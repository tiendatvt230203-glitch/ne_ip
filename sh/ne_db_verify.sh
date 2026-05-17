#!/usr/bin/env bash
# Verify PostgreSQL connection and that the NEW schema (ne_*) is present.
# Does not use legacy xdp_* tables or xdpdb user.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=db_env.sh
source "${SCRIPT_DIR}/db_env.sh"
network_encryptor_load_db_env

echo "=== ne_db_verify ==="
echo "postgres://${POSTGRES_USER}@${POSTGRES_SERVER}:${POSTGRES_PORT}/${POSTGRES_DB}"
echo "NE code loads: ne_profiles, ne_policies, ne_lan, ne_wan (NOT xdp_* / NOT xdpdb)"
echo

ne_psql -c "SELECT version();" >/dev/null
echo "[OK] PostgreSQL connection"

VERIFY_SQL=$(cat <<'EOSQL'
SELECT
  EXISTS (SELECT 1 FROM information_schema.tables
          WHERE table_schema = 'public' AND table_name = 'ne_profiles') AS has_ne_profiles,
  EXISTS (SELECT 1 FROM information_schema.tables
          WHERE table_schema = 'public' AND table_name = 'ne_policies') AS has_ne_policies,
  EXISTS (SELECT 1 FROM information_schema.tables
          WHERE table_schema = 'public' AND table_name = 'ne_lan') AS has_ne_lan,
  EXISTS (SELECT 1 FROM information_schema.tables
          WHERE table_schema = 'public' AND table_name = 'ne_wan') AS has_ne_wan,
  EXISTS (SELECT 1 FROM information_schema.tables
          WHERE table_schema = 'public' AND table_name = 'xdp_profiles') AS has_old_xdp_profiles,
  EXISTS (SELECT 1 FROM information_schema.tables
          WHERE table_schema = 'public' AND table_name = 'xdp_config') AS has_old_xdp_config;
EOSQL
)

RESULT=$(ne_psql -t -A -F'|' -c "${VERIFY_SQL}")
IFS='|' read -r has_np has_npol has_lan has_wan has_old_prof has_old_cfg <<< "${RESULT}"

fail=0
for label val in \
  ne_profiles "${has_np}" \
  ne_policies "${has_npol}" \
  ne_lan "${has_lan}" \
  ne_wan "${has_wan}"; do
  if [ "${val}" != "t" ]; then
    echo "[FAIL] missing table: ${label} — run: sh/xdp_init_db.sh" >&2
    fail=1
  else
    echo "[OK] table ${label}"
  fi
done

if [ "${has_old_prof}" = "t" ] || [ "${has_old_cfg}" = "t" ]; then
  echo "[WARN] legacy xdp_* tables still exist in this database (daemon ignores them)"
  echo "       NE only reads ne_* — you are NOT loading the old xdpdb schema at runtime"
fi

if [ "${fail}" -ne 0 ]; then
  exit 1
fi

echo
echo "--- ne_profiles (id, name) ---"
ne_psql -c "SELECT id, name FROM ne_profiles ORDER BY id;"

echo
echo "--- row counts ---"
ne_psql -c "
SELECT 'ne_profiles' AS tbl, COUNT(*)::text AS rows FROM ne_profiles
UNION ALL SELECT 'ne_policies', COUNT(*)::text FROM ne_policies
UNION ALL SELECT 'ne_lan', COUNT(*)::text FROM ne_lan
UNION ALL SELECT 'ne_wan', COUNT(*)::text FROM ne_wan
ORDER BY tbl;
"

echo
echo "[OK] Database ready for network-encryptor-ip (ne_* schema)"
