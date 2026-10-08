#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
export STORE_DIR="${STORE_DIR:-$HERE/data}"
if [ -z "${TOP20_SEED+x}" ] && [ -f "$HERE/bin/top20-seed.json" ]; then
  export TOP20_SEED="$HERE/bin/top20-seed.json"
fi
exec "$HERE/bin/server"
