#include <bpf/libbpf.h>
#include <libpq-fe.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <pthread.h>
#include <unistd.h>

#include "config.h"
#include "db_env.h"
#include "db_runtime.h"
#include "forwarder.h"
#include "main_diag.h"

#define NOTIFY_CHANNEL "xdp_start"
#define MAX_ACTIVE_PROFILE_IDS 32

struct runtime_state {
    pthread_t thread;
    int has_thread;
    int running;
    struct forwarder fwd;
    struct app_config cfg_slots[2];
    int active_slot;
};

static void usage(const char *prog) {
    fprintf(stderr,
            "Usage:\n"
            "  %s -id <ne_profiles.id>\n"
            "  %s\n",
            prog, prog);
}

static int parse_startup_profile_id(int argc, char **argv, int *out_id) {
    *out_id = -1;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-id") == 0) {
            if (*out_id >= 0) {
                fprintf(stderr, "[FATAL] -id specified more than once\n");
                return -1;
            }
            if (i + 1 >= argc) {
                fprintf(stderr, "[FATAL] -id requires ne_profiles.id\n");
                return -1;
            }
            char *end = NULL;
            long v = strtol(argv[i + 1], &end, 10);
            if (!end || *end != '\0' || v < 0 || v > INT_MAX) {
                fprintf(stderr, "[FATAL] invalid ne_profiles.id: %s\n", argv[i + 1]);
                return -1;
            }
            *out_id = (int)v;
            i++;
            continue;
        }
        fprintf(stderr, "[FATAL] unknown option: %s\n", argv[i]);
        return -1;
    }
    return 0;
}

static int active_ids_add(int *active_ids, int *active_id_count, int id) {
    for (int i = 0; i < *active_id_count; i++) {
        if (active_ids[i] == id)
            return 0;
    }
    if (*active_id_count >= MAX_ACTIVE_PROFILE_IDS) {
        fprintf(stderr, "[WARN] active profile set is full, ignoring id=%d\n", id);
        return -1;
    }
    active_ids[(*active_id_count)++] = id;
    return 0;
}

static int libbpf_print_silent(enum libbpf_print_level level,
                               const char *format,
                               va_list args) {
    (void)level;
    (void)format;
    (void)args;
    return 0;
}

static void *forwarder_thread_main(void *arg) {
    forwarder_pin_cpu();
    struct runtime_state *rt = (struct runtime_state *)arg;
    if (forwarder_init(&rt->fwd, &rt->cfg_slots[rt->active_slot]) != 0) {
        fprintf(stderr, "[FATAL] forwarder_init failed for merged active configs\n");
        rt->running = 0;
        return NULL;
    }
    fprintf(stderr, "[RUNTIME] forwarder_init OK locals=%d wans=%d\n",
            rt->fwd.local_count, rt->fwd.wan_count);
    rt->running = 1;
    forwarder_run(&rt->fwd);
    forwarder_cleanup(&rt->fwd);
    rt->running = 0;
    return NULL;
}

static int runtime_start(struct runtime_state *rt, const struct app_config *cfg) {
    rt->active_slot = 0;
    rt->cfg_slots[rt->active_slot] = *cfg;
    rt->running = 0;
    if (pthread_create(&rt->thread, NULL, forwarder_thread_main, rt) != 0) {
        fprintf(stderr, "[FATAL] failed to create forwarder thread\n");
        return -1;
    }
    rt->has_thread = 1;
    return 0;
}

static int apply_active_configs(struct runtime_state *rt, const int *active_ids,
                                int active_id_count, int trigger_id) {
    struct app_config merged_cfg;
    if (build_merged_config(&merged_cfg, active_ids, active_id_count, NULL) != 0) {
        fprintf(stderr, "[FATAL] failed to build config for ne_profiles.id=%d\n", trigger_id);
        return -1;
    }

    main_diag_log_loaded_config(&merged_cfg, trigger_id);

    if (!rt->has_thread) {
        if (runtime_start(rt, &merged_cfg) != 0) {
            fprintf(stderr, "[FATAL] failed to start forwarder\n");
            return -1;
        }
        fprintf(stderr, "[OK] loaded ne_profiles.id=%d profiles=%d\n",
                trigger_id, active_id_count);
        return 0;
    }

    int next_slot = 1 - rt->active_slot;
    rt->cfg_slots[next_slot] = merged_cfg;
    if (forwarder_reload_config(&rt->fwd, &rt->cfg_slots[next_slot]) == 0) {
        rt->active_slot = next_slot;
        fprintf(stderr, "[OK] hot-reloaded %d active profile(s) (trigger id=%d)\n",
                active_id_count, trigger_id);
        return 0;
    }

    fprintf(stderr,
            "[WARN] hot reload rejected for ne_profiles.id=%d; keeping current runtime\n",
            trigger_id);
    return -1;
}

int main(int argc, char **argv) {
    setbuf(stderr, NULL);

    if (load_ne_env() != 0) {
        fprintf(stderr, "[FATAL] DB env not loaded from " NE_ENV_FILE "\n");
        return 1;
    }
    struct ne_postgres_conn pg;
    if (ne_postgres_conn_fill(&pg) != 0) {
        fprintf(stderr,
                "[FATAL] Missing POSTGRES_SERVER/PORT/USER/DB/PASSWORD in " NE_ENV_FILE "\n");
        return 1;
    }

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }

    int startup_id = -1;
    if (parse_startup_profile_id(argc, argv, &startup_id) != 0) {
        usage(argv[0]);
        return 1;
    }

    libbpf_set_print(libbpf_print_silent);
    forwarder_pin_cpu();
    PGconn *listen_conn = PQconnectdbParams(pg.keywords, pg.values, 0);
    if (PQstatus(listen_conn) != CONNECTION_OK) {
        fprintf(stderr, "[FATAL] DB connection failed: %s", PQerrorMessage(listen_conn));
        PQfinish(listen_conn);
        return 1;
    }
    PQclear(PQexec(listen_conn, "LISTEN " NOTIFY_CHANNEL));

    struct runtime_state rt;
    memset(&rt, 0, sizeof(rt));
    int active_ids[MAX_ACTIVE_PROFILE_IDS];
    int active_id_count = 0;

    if (startup_id >= 0) {
        fprintf(stderr, "[STARTUP] loading ne_profiles.id=%d from DB\n", startup_id);
        if (active_ids_add(active_ids, &active_id_count, startup_id) != 0 ||
            apply_active_configs(&rt, active_ids, active_id_count, startup_id) != 0) {
            PQfinish(listen_conn);
            return 1;
        }
    }

    for (;;) {
        int pq_fd = PQsocket(listen_conn);
        if (pq_fd < 0) {
            PQreset(listen_conn);
            PQclear(PQexec(listen_conn, "LISTEN " NOTIFY_CHANNEL));
            usleep(200000);
            continue;
        }
        fd_set rfds;
        FD_ZERO(&rfds);
        FD_SET(pq_fd, &rfds);

        if (select(pq_fd + 1, &rfds, NULL, NULL, NULL) < 0)
            continue;

        PQconsumeInput(listen_conn);
        PGnotify *notify;
        while ((notify = PQnotifies(listen_conn)) != NULL) {
            int id = atoi(notify->extra);
            if (id <= 0) {
                fprintf(stderr, "[WARN] ignoring NOTIFY with invalid id payload: \"%s\"\n",
                        notify->extra ? notify->extra : "");
                PQfreemem(notify);
                continue;
            }

            (void)active_ids_add(active_ids, &active_id_count, id);
            apply_active_configs(&rt, active_ids, active_id_count, id);
            PQfreemem(notify);
        }

        if (PQstatus(listen_conn) != CONNECTION_OK) {
            PQreset(listen_conn);
            PQclear(PQexec(listen_conn, "LISTEN " NOTIFY_CHANNEL));
        }
    }
}
