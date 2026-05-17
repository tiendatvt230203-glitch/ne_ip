#include <bpf/libbpf.h>
#include <libpq-fe.h>
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
            "  %s               # daemon mode (LISTEN %s)\n",
            prog, NOTIFY_CHANNEL);
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
    fprintf(stderr,
            "[RUNTIME] forwarder_init OK — locals=%d wans=%d (XDP attach runs inside init; "
            "if locals>0 and no prog on LAN iface, check journal for bpf/XSK errors above)\n",
            rt->fwd.local_count,
            rt->fwd.wan_count);
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

int main(int argc, char **argv) {
    setbuf(stderr, NULL);

    load_env_from_file("/opt/db.env");
    struct ne_postgres_conn pg;
    if (ne_postgres_conn_fill(&pg) != 0) {
        fprintf(stderr,
                "[FATAL] Missing POSTGRES_SERVER/PORT/USER/DB/PASSWORD in /opt/db.env\n");
        return 1;
    }

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }

    if (argc > 1) {
        fprintf(stderr, "[FATAL] unknown option (use pg_notify to load config)\n");
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
    int active_ids[32];
    int active_id_count = 0;

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
            int exists = 0;
            for (int i = 0; i < active_id_count; i++) {
                if (active_ids[i] == id) {
                    exists = 1;
                    break;
                }
            }
            if (!exists) {
                if (active_id_count >= (int)(sizeof(active_ids) / sizeof(active_ids[0]))) {
                    fprintf(stderr, "[WARN] active config set is full, ignoring id=%d\n", id);
                    PQfreemem(notify);
                    continue;
                }
                active_ids[active_id_count++] = id;
            }

            struct app_config merged_cfg;
            if (build_merged_config(&merged_cfg, active_ids, active_id_count, pg.values[4]) == 0) {
                main_diag_log_loaded_config(&merged_cfg, id);
                if (!rt.has_thread) {
                    if (runtime_start(&rt, &merged_cfg) != 0) {
                        fprintf(stderr, "[FATAL] failed to start merged runtime\n");
                    } else {
                        fprintf(stderr,
                                "[OK] NOTIFY handled; forwarder thread started (%d active config(s)) — "
                                "init finishes asynchronously; confirm \"[RUNTIME] forwarder_init OK\"\n",
                                active_id_count);
                    }
                } else {
                    int next_slot = 1 - rt.active_slot;
                    rt.cfg_slots[next_slot] = merged_cfg;
                    if (forwarder_reload_config(&rt.fwd, &rt.cfg_slots[next_slot]) == 0) {
                        rt.active_slot = next_slot;
                        fprintf(stderr, "[OK] Hot-reloaded merged runtime with %d active config(s)\n", active_id_count);
                    } else {
                        fprintf(stderr,
                                "[WARN] hot reload rejected for safety; keep current runtime unchanged "
                                "(prevents traffic interruption)\n");
                    }
                }
            } else {
                fprintf(stderr, "[FATAL] failed to build merged config set after notify id=%d\n", id);
            }
            PQfreemem(notify);
        }

        if (PQstatus(listen_conn) != CONNECTION_OK) {
            PQreset(listen_conn);
            PQclear(PQexec(listen_conn, "LISTEN " NOTIFY_CHANNEL));
        }
    }
}
