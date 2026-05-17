#include <bpf/libbpf.h>
#include <libpq-fe.h>
#include <limits.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <pthread.h>
#include <unistd.h>

#include "config.h"
#include "db_env.h"
#include "db_runtime.h"
#include "forwarder.h"
#include "main_diag.h"

#define NOTIFY_CHANNEL "xdp_start"
#define MAX_ACTIVE_PROFILE_IDS 32

static char g_ctrl_sock[108];

static void ctrl_sock_path(void) {
    snprintf(g_ctrl_sock, sizeof(g_ctrl_sock),
             "/tmp/network-encryptor-%u.sock", (unsigned)getuid());
}

static int ctrl_parse_start(const char *line, int *id_out) {
    while (line && (*line == ' ' || *line == '\t'))
        line++;
    int id = -1;
    if (!line || (sscanf(line, "start %d", &id) != 1 && sscanf(line, "%d", &id) != 1))
        return -1;
    if (id <= 0)
        return -1;
    *id_out = id;
    return 0;
}

static int ctrl_server_start(void) {
    ctrl_sock_path();
    unlink(g_ctrl_sock);
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_ctrl_sock);
    if (bind(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    chmod(g_ctrl_sock, 0600);
    if (listen(fd, 4) != 0) {
        close(fd);
        unlink(g_ctrl_sock);
        return -1;
    }
    return fd;
}

static int ctrl_server_poll(int listen_fd, int *profile_id_out) {
    int cfd = accept(listen_fd, NULL, NULL);
    if (cfd < 0)
        return -1;
    char buf[64];
    ssize_t n = recv(cfd, buf, sizeof(buf) - 1, 0);
    int id = -1;
    if (n > 0 && ctrl_parse_start((buf[n] = '\0', buf), &id) == 0) {
        *profile_id_out = id;
        send(cfd, "OK\n", 3, 0);
        close(cfd);
        return 0;
    }
    send(cfd, "ERR\n", 4, 0);
    close(cfd);
    return -1;
}

static int ctrl_client_request_start(int profile_id) {
    if (profile_id <= 0)
        return -1;
    ctrl_sock_path();
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0)
        return -1;
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", g_ctrl_sock);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(fd);
        return -1;
    }
    char msg[32];
    int mlen = snprintf(msg, sizeof(msg), "start %d\n", profile_id);
    if (send(fd, msg, (size_t)mlen, 0) != mlen) {
        close(fd);
        return -1;
    }
    char resp[16];
    ssize_t rn = recv(fd, resp, sizeof(resp) - 1, 0);
    close(fd);
    if (rn <= 0)
        return -1;
    resp[rn] = '\0';
    return strncmp(resp, "OK", 2) == 0 ? 0 : -1;
}

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
            "  %s                       daemon: wait on this terminal (no XDP yet)\n"
            "  %s -id <ne_profiles.id>   tell daemon to load+run (logs on daemon screen)\n",
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

static int daemon_handle_start(struct runtime_state *rt,
                               int *active_ids, int *active_id_count,
                               int profile_id, const char *via) {
    fprintf(stderr, "[STARTUP] loading ne_profiles.id=%d from DB (%s)\n",
            profile_id, via);

    if (!rt->has_thread)
        *active_id_count = 0;

    if (active_ids_add(active_ids, active_id_count, profile_id) != 0)
        return -1;
    return apply_active_configs(rt, active_ids, *active_id_count, profile_id);
}

int main(int argc, char **argv) {
    setbuf(stderr, NULL);

    if (argc > 1 && (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0)) {
        usage(argv[0]);
        return 0;
    }

    int startup_id = -1;
    if (parse_startup_profile_id(argc, argv, &startup_id) != 0) {
        usage(argv[0]);
        return 1;
    }

    if (startup_id >= 0) {
        if (ctrl_client_request_start(startup_id) == 0) {
            fprintf(stderr,
                    "[OK] ne_profiles.id=%d sent to daemon (logs on daemon terminal)\n",
                    startup_id);
            return 0;
        }
    }

    if (load_ne_env() != 0) {
        fprintf(stderr, "[FATAL] DB env not loaded from " NE_ENV_FILE "\n");
        return 1;
    }

    if (startup_id >= 0) {
        fprintf(stderr,
                "[WARN] no daemon listening; starting in this process\n");
    }

    struct ne_postgres_conn pg;
    if (ne_postgres_conn_fill(&pg) != 0) {
        fprintf(stderr,
                "[FATAL] Missing POSTGRES_SERVER/PORT/USER/DB/PASSWORD in " NE_ENV_FILE "\n");
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

    int ctrl_fd = -1;
    if (startup_id < 0) {
        ctrl_fd = ctrl_server_start();
        if (ctrl_fd < 0) {
            fprintf(stderr, "[FATAL] control socket failed (is another daemon running?)\n");
            PQfinish(listen_conn);
            return 1;
        }
        fprintf(stderr,
                "[WAIT] idle — no config loaded, no XDP/forwarder (traffic unchanged).\n"
                "[WAIT] on this terminal: run in another shell: %s -id <ne_profiles.id>\n",
                argv[0]);
    } else {
        if (daemon_handle_start(&rt, active_ids, &active_id_count,
                                startup_id, "foreground") != 0) {
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
        int maxfd = pq_fd;
        if (ctrl_fd >= 0) {
            FD_SET(ctrl_fd, &rfds);
            if (ctrl_fd > maxfd)
                maxfd = ctrl_fd;
        }

        if (select(maxfd + 1, &rfds, NULL, NULL, NULL) < 0)
            continue;

        if (ctrl_fd >= 0 && FD_ISSET(ctrl_fd, &rfds)) {
            int profile_id = -1;
            if (ctrl_server_poll(ctrl_fd, &profile_id) == 0 && profile_id > 0) {
                if (daemon_handle_start(&rt, active_ids, &active_id_count,
                                        profile_id, "-id command") != 0) {
                    fprintf(stderr,
                            "[FATAL] failed to start for ne_profiles.id=%d\n", profile_id);
                }
            }
        }

        if (FD_ISSET(pq_fd, &rfds)) {
            PQconsumeInput(listen_conn);
            PGnotify *notify;
            while ((notify = PQnotifies(listen_conn)) != NULL) {
                int id = atoi(notify->extra);
                if (id <= 0) {
                    fprintf(stderr,
                            "[WARN] ignoring NOTIFY with invalid id payload: \"%s\"\n",
                            notify->extra ? notify->extra : "");
                } else if (!rt.has_thread) {
                    fprintf(stderr,
                            "[WAIT] NOTIFY id=%d ignored (idle). Run: %s -id %d\n",
                            id, argv[0], id);
                } else {
                    (void)active_ids_add(active_ids, &active_id_count, id);
                    apply_active_configs(&rt, active_ids, active_id_count, id);
                }
                PQfreemem(notify);
            }
        }

        if (PQstatus(listen_conn) != CONNECTION_OK) {
            PQreset(listen_conn);
            PQclear(PQexec(listen_conn, "LISTEN " NOTIFY_CHANNEL));
        }
    }
}
