#!/usr/bin/env bash
# Load every sql_options/<NN>_*.sql (skip *_peer.sql) into ne_* tables.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=db_env.sh
source "${SCRIPT_DIR}/db_env.sh"
network_encryptor_load_db_env

SQL_DIR="${NETWORK_ENCRYPTOR_ROOT}/sql_options"
LOAD_ONE="${SCRIPT_DIR}/xdp_load_option.sh"

if [ ! -d "${SQL_DIR}" ]; then
  echo "[FATAL] missing ${SQL_DIR}" >&2
  exit 1
fi

echo "=== ne_load_all_profiles ==="
echo "postgres://${POSTGRES_USER}@${POSTGRES_SERVER}:${POSTGRES_PORT}/${POSTGRES_DB}"
echo

bash "${SCRIPT_DIR}/ne_db_verify.sh"
echo

shopt -s nullglob
SQL_FILES=("${SQL_DIR}"/*.sql)
shopt -u nullglob

if [ "${#SQL_FILES[@]}" -eq 0 ]; then
  echo "[FATAL] no *.sql in ${SQL_DIR}" >&2
  exit 1
fi

declare -A IDS=()
for sql_file in "${SQL_FILES[@]}"; do
  base="$(basename "${sql_file}")"
  if [[ "${base}" == *_peer.sql ]]; then
    continue
  fi
  if [[ "${base}" =~ ^([0-9]+)_.*\.sql$ ]]; then
    id="${BASH_REMATCH[1]}"
    id=$((10#${id}))
    IDS["${id}"]=1
  fi
done

if [ "${#IDS[@]}" -eq 0 ]; then
  echo "[FATAL] no files matching <NN>_*.sql (excluding *_peer.sql)" >&2
  exit 1
fi

mapfile -t SORTED_IDS < <(printf '%s\n' "${!IDS[@]}" | sort -n)

for id in "${SORTED_IDS[@]}"; do
  echo ">>> load ne_profiles.id=${id}"
  bash "${LOAD_ONE}" "${id}"
  echo
done

echo "OK loaded profile ids: ${SORTED_IDS[*]}"
echo "Peer NE: sh/xdp_load_option.sh <id> <NN>_..._peer.sql"
