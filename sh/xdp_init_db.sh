#!/usr/bin/env bash
# Create ne_* tables in existing POSTGRES_DB (shared BE database, user sep).
# Env: /opt/db.env — POSTGRES_SERVER, POSTGRES_PORT, POSTGRES_USER, POSTGRES_DB, POSTGRES_PASSWORD
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=db_env.sh
source "${SCRIPT_DIR}/db_env.sh"
network_encryptor_load_db_env

SCHEMA_FILE="${NETWORK_ENCRYPTOR_ROOT}/schema.sql"
if [ ! -f "${SCHEMA_FILE}" ]; then
  echo "[FATAL] schema not found: ${SCHEMA_FILE}" >&2
  exit 1
fi

echo "=== ne_init_schema ==="
echo "Target: postgres://${POSTGRES_USER}@${POSTGRES_SERVER}:${POSTGRES_PORT}/${POSTGRES_DB}"
echo "Tables: ne_profiles, ne_policies, ne_lan, ne_wan (NOT legacy xdp_*)"
echo "Root:   ${NETWORK_ENCRYPTOR_ROOT}"
echo

echo "[1/2] Apply schema.sql"
ne_psql_file "${SCHEMA_FILE}"

echo "[2/2] Verify ne_* schema"
bash "${SCRIPT_DIR}/ne_db_verify.sh"

echo
echo "Next:"
echo "  sh/xdp_load_option.sh <ne_profiles.id>     # seed + notify daemon"
echo "  sudo ./bin/network-encryptor              # LISTEN xdp_start"
echo "  sh/ne_notify_start.sh <id>                # reload without re-seed"
