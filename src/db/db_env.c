#include "../../inc/db_env.h"

#include <limits.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>

static int ne_env_key_allowed(const char *key) {
    static const char *allowed[] = {
        "POSTGRES_SERVER",
        "POSTGRES_PORT",
        "POSTGRES_USER",
        "POSTGRES_DB",
        "POSTGRES_PASSWORD",
        NULL
    };
    if (!key || !key[0])
        return 0;
    for (int i = 0; allowed[i]; i++) {
        if (strcmp(key, allowed[i]) == 0)
            return 1;
    }
    return 0;
}

static void strip_env_quotes(char *val) {
    size_t len = strlen(val);
    if (len >= 2 && val[0] == '"' && val[len - 1] == '"') {
        val[len - 1] = '\0';
        memmove(val, val + 1, len - 1);
        return;
    }
    if (len >= 2 && val[0] == '\'' && val[len - 1] == '\'') {
        val[len - 1] = '\0';
        memmove(val, val + 1, len - 1);
    }
}

static void ne_sync_pgpassword(void) {
    const char *pass = getenv("POSTGRES_PASSWORD");
    if (pass && pass[0])
        setenv("PGPASSWORD", pass, 1);
}

static int ne_env_file_readable(const char *path) {
    return path && path[0] && access(path, R_OK) == 0;
}

void load_env_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) {
        fprintf(stderr, "[ENV] Could not open env file: %s\n", path);
        return;
    }

    fprintf(stderr, "[ENV] loading POSTGRES_* from %s\n", path);

    char line[2048];
    while (fgets(line, sizeof(line), f)) {
        char *p = line;
        while (*p == ' ' || *p == '\t')
            p++;
        if (*p == '\0' || *p == '\n' || *p == '#')
            continue;
        if (strncmp(p, "POSTGRES_", 9) != 0)
            continue;

        char *eq = strchr(p, '=');
        if (!eq)
            continue;

        *eq = '\0';
        char *key = p;
        char *val = eq + 1;

        char *end = key + strlen(key) - 1;
        while (end > key && (*end == ' ' || *end == '\t'))
            *end-- = '\0';

        while (*val == ' ' || *val == '\t')
            val++;

        size_t len = strlen(val);
        while (len > 0 && (val[len - 1] == '\n' || val[len - 1] == '\r')) {
            val[--len] = '\0';
        }

        strip_env_quotes(val);

        if (*key && *val && ne_env_key_allowed(key))
            setenv(key, val, 1);
    }

    fclose(f);
    ne_sync_pgpassword();
}

int load_env_default(void) {
    const char *override = getenv("DB_ENV_FILE");
    static const char *fallbacks[] = {
        NE_DEFAULT_ENV_FILE,
        "/opt/db.env",
        NULL,
    };

    if (ne_env_file_readable(override)) {
        load_env_from_file(override);
        return 0;
    }

    for (int i = 0; fallbacks[i]; i++) {
        if (ne_env_file_readable(fallbacks[i])) {
            load_env_from_file(fallbacks[i]);
            return 0;
        }
    }

    fprintf(stderr,
            "[ENV] No env file found (tried DB_ENV_FILE, %s, /opt/db.env)\n",
            NE_DEFAULT_ENV_FILE);
    return -1;
}

int ne_postgres_conn_fill(struct ne_postgres_conn *out) {
    if (!out)
        return -1;

    const char *host = getenv("POSTGRES_SERVER");
    const char *port = getenv("POSTGRES_PORT");
    const char *user = getenv("POSTGRES_USER");
    const char *dbname = getenv("POSTGRES_DB");
    const char *pass = resolve_db_password();

    if (!host || !host[0] || !port || !port[0] || !user || !user[0] ||
        !dbname || !dbname[0] || !pass || !pass[0]) {
        fprintf(stderr,
                "[DB] Missing POSTGRES_SERVER/PORT/USER/DB/PASSWORD "
                "(expected in " NE_DEFAULT_ENV_FILE ")\n");
        return -1;
    }

    static const char *kw[] = {
        "host", "port", "dbname", "user", "password", "connect_timeout", NULL
    };
    for (int i = 0; kw[i]; i++)
        out->keywords[i] = kw[i];
    out->values[0] = host;
    out->values[1] = port;
    out->values[2] = dbname;
    out->values[3] = user;
    out->values[4] = pass;
    out->values[5] = "10";
    out->values[6] = NULL;
    return 0;
}

const char *resolve_db_password(void) {
    const char *p = getenv("POSTGRES_PASSWORD");
    if (p && *p) return p;
    p = getenv("PGPASSWORD");
    if (p && *p) return p;
    return NULL;
}

int parse_config_id_arg(const char *s, int *out) {
    if (!s || !*s) return -1;
    for (const char *p = s; *p; p++) {
        if (*p < '0' || *p > '9') return -1;
    }
    long v = strtol(s, NULL, 10);
    if (v < 0 || v > INT_MAX) return -1;
    *out = (int)v;
    return 0;
}
