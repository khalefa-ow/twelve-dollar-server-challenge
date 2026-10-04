#!/usr/bin/env bash
# Runs once as root on a clean Ubuntu 24.04: installs the pinned Bun release (checksum-verified)
# to /usr/local/bin/bun.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
BUN_VERSION=1.4.2
BUN_SHA256=36368faef7527875d5ffa52e53cd48021741f2a83eb6208a8dd64068d422a913  # bun-linux-x64.zip

apt-get update
apt-get install -y --no-install-recommends curl ca-certificates unzip
tmp="$(mktemp -d)"; trap 'rm -rf "$tmp"' EXIT
curl -fsSL -o "$tmp/bun.zip" "https://github.com/oven-sh/bun/releases/download/bun-v$BUN_VERSION/bun-linux-x64.zip"
echo "$BUN_SHA256  $tmp/bun.zip" | sha256sum -c --quiet
unzip -q "$tmp/bun.zip" -d "$tmp"
install -m 0755 "$tmp/bun-linux-x64/bun" /usr/local/bin/bun
bun --version
