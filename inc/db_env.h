#ifndef DB_ENV_H
#define DB_ENV_H

#define NE_DEFAULT_ENV_FILE "/opt/SEP/be/.env"

struct ne_postgres_conn {
    const char *keywords[7];
    const char *values[7];
};

void load_env_from_file(const char *path);
int load_env_default(void);
int ne_postgres_conn_fill(struct ne_postgres_conn *out);
const char *resolve_db_password(void);
int parse_config_id_arg(const char *s, int *out);

#endif
