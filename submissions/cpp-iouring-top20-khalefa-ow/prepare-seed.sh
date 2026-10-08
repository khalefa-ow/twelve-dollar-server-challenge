#!/usr/bin/env bash
# Optional preparation: generate TSVs, keep only the newest 20 posts and their memberships.
set -euo pipefail
cd "$(dirname "$0")"
mkdir -p bin
seed_tmp=$(mktemp -d)
trap 'rm -rf "$seed_tmp"' EXIT
node ../../seed/generate.mjs "$seed_tmp"
python3 tools/export_seed.py "$seed_tmp" bin/top20-seed.json
cp "$seed_tmp/tokens.json" bin/tokens.json
echo "Prepared $PWD/bin/top20-seed.json and $PWD/bin/tokens.json"
