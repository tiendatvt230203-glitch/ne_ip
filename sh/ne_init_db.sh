#!/usr/bin/env bash
# Apply schema.sql (ne_* tables) to POSTGRES_DB — same as xdp_init_db.sh
set -euo pipefail
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/xdp_init_db.sh" "$@"
