#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=db_env.sh
source "${SCRIPT_DIR}/db_env.sh"
network_encryptor_load_db_env

if [ "$#" -ne 1 ]; then
  echo "Usage: $0 <ne_profiles.id>" >&2
  exit 1
fi

PROFILE_ID="$1"
if ! [[ "${PROFILE_ID}" =~ ^[0-9]+$ ]]; then
  echo "profile_id must be digits (ne_profiles.id)" >&2
  exit 1
fi

ne_psql -c "SELECT 1 FROM ne_profiles WHERE id = ${PROFILE_ID}::int;" >/dev/null
ne_psql -c "SELECT pg_notify('xdp_start', '${PROFILE_ID}');"
echo "OK pg_notify xdp_start ${PROFILE_ID} on ${POSTGRES_DB}@${POSTGRES_SERVER}"
