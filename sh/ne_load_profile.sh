#!/usr/bin/env bash
# Load sql_options seed + pg_notify — same as xdp_load_option.sh
set -euo pipefail
exec "$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/xdp_load_option.sh" "$@"
