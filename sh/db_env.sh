_sh_db_env_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export NETWORK_ENCRYPTOR_ROOT="$(cd "${_sh_db_env_dir}/.." && pwd)"

network_encryptor_read_env_kv() {
  local file="$1" key="$2"
  local line raw
  line=$(grep -E "^[[:space:]]*${key}=" "$file" 2>/dev/null | head -1) || return 1
  raw="${line#*=}"
  raw="${raw#"${raw%%[![:space:]]*}"}"
  if [ "${#raw}" -ge 2 ]; then
    case "$raw" in
      \"*) raw="${raw#\"}"; raw="${raw%\"}" ;;
      \'*) raw="${raw#\'}"; raw="${raw%\'}" ;;
    esac
  fi
  printf '%s' "$raw"
}

network_encryptor_load_db_env_from_file() {
  local file="$1"
  local v
  v=$(network_encryptor_read_env_kv "$file" POSTGRES_SERVER) && export POSTGRES_SERVER="$v"
  v=$(network_encryptor_read_env_kv "$file" POSTGRES_PORT) && export POSTGRES_PORT="$v"
  v=$(network_encryptor_read_env_kv "$file" POSTGRES_USER) && export POSTGRES_USER="$v"
  v=$(network_encryptor_read_env_kv "$file" POSTGRES_DB) && export POSTGRES_DB="$v"
  v=$(network_encryptor_read_env_kv "$file" POSTGRES_PASSWORD) && export POSTGRES_PASSWORD="$v"
}

network_encryptor_load_db_env() {
  local env_file=""
  if [ -n "${DB_ENV_FILE:-}" ] && [ -f "${DB_ENV_FILE}" ]; then
    env_file="${DB_ENV_FILE}"
  elif [ -f "/opt/SEP/be/.env" ]; then
    env_file="/opt/SEP/be/.env"
  elif [ -f "${NETWORK_ENCRYPTOR_ROOT}/.db.env" ]; then
    env_file="${NETWORK_ENCRYPTOR_ROOT}/.db.env"
  elif [ -f "/opt/db.env" ]; then
    env_file="/opt/db.env"
  else
    echo "[FATAL] DB env not found. Use: DB_ENV_FILE=path or /opt/SEP/be/.env" >&2
    return 1
  fi

  echo "[ENV] loading POSTGRES_* from ${env_file}"
  network_encryptor_load_db_env_from_file "${env_file}"

  : "${POSTGRES_SERVER:?POSTGRES_SERVER is required}"
  : "${POSTGRES_PORT:?POSTGRES_PORT is required}"
  : "${POSTGRES_USER:?POSTGRES_USER is required}"
  : "${POSTGRES_DB:?POSTGRES_DB is required}"
  : "${POSTGRES_PASSWORD:?POSTGRES_PASSWORD is required}"

  export PGPASSWORD="${POSTGRES_PASSWORD}"
  export POSTGRES_SERVER POSTGRES_PORT POSTGRES_USER POSTGRES_DB POSTGRES_PASSWORD
  return 0
}

ne_psql() {
  psql -v ON_ERROR_STOP=1 -h "${POSTGRES_SERVER}" -p "${POSTGRES_PORT}" \
    -U "${POSTGRES_USER}" -d "${POSTGRES_DB}" "$@"
}

ne_psql_file() {
  local sql_file="$1"
  ne_psql -f "${sql_file}"
}
