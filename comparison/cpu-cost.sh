#!/usr/bin/env bash
# CPU cost per request, per endpoint, for several submissions.
#
#   bash comparison/cpu-cost.sh > comparison/per-request-cpu.txt
#
# Each server is pinned to core 0 on a fresh copy of seed/feed.db. cost.mjs (Node >= 18) runs an
# 8-second closed loop of one request type over 200 keep-alive connections from the other cores and
# reads the server's user/sys CPU from /proc. "mix" uses bench/load.js's ratios (feed 1 : post 1 :
# like 0.15 : create 0.02). The client tops out at ~10-15k req/s, so these are costs, not max throughput.
#
# Env: IMPLS (space-separated folder names), HOST/PORT (default 0.0.0.0:80, serving directly as in
# the challenge; binding port 80 needs net.ipv4.ip_unprivileged_port_start <= 80 or root).
# HOT=1 makes post/like requests target the posts currently on /feed instead of ids 1..500000.
# Bun must be on PATH for the Bun server.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
read -r -a IMPLS <<< "${IMPLS:-cpp-epoll-khalefa-ow c-fio-tbsvttr cpp-uwebsockets-tbsvttr typescript-bun-khalefa-ow python-fastapi-cknutson12}"
DB="$(mktemp -d)/cost.db"
export JWT_SECRET=twelve-dollar-challenge HOST="${HOST:-0.0.0.0}" PORT="${PORT:-80}"
ulimit -n 65535
cd "$ROOT"

echo "#### per-request CPU (server pinned to core 0; 200 keep-alive conns; 8 s per endpoint)"
for impl in "${IMPLS[@]}"; do
  cp seed/feed.db "$DB"; rm -rf "$DB"-* "$DB".memstore
  SQLITE_PATH="$DB" setsid taskset -c 0 bash "submissions/$impl/start.sh" > /dev/null 2>&1 &
  for _ in $(seq 60); do curl -sf "127.0.0.1:$PORT/health" >/dev/null && break; sleep 0.5; done
  pid=$(ss -ltnp "( sport = :$PORT )" | grep -oP 'pid=\K[0-9]+' | head -1)
  echo "== $impl"
  for m in health post feed like create mix; do P="$PORT" taskset -c 1-7 node comparison/cost.mjs seed/tokens.json "$pid" "$m"; done
  kill -- -"$(ps -o sid= -p "$pid" | tr -d ' ')" 2>/dev/null; sleep 2
done
