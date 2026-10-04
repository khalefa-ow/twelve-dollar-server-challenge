#!/usr/bin/env bash
# Nothing to compile or download: Bun runs server.ts directly and the app has no dependencies.
# This only checks that the pinned Bun is installed.
set -euo pipefail
BUN="$(command -v bun || echo /usr/local/bin/bun)"
"$BUN" --version
