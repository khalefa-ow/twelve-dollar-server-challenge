#!/usr/bin/env bash
# k6 ladder for several submissions in one session, to compare them under identical conditions.
#
#   bash comparison/ladder.sh [passes] > comparison/ladder-results.tsv
#
# Each run: fresh copy of seed/feed.db, server pinned to core 0 (stand-in for the 1-vCPU droplet),
# k6 on the remaining cores, bench/load.js with a 30 s ramp and 60 s hold (shorter than the official
# 5-minute hold). Server order rotates per level and pass so slow drift doesn't hit one server.
# Server CPU% is the server process's own utime+stime over the k6 run (all threads).
#
# Env: K6 (default: k6 on PATH), IMPLS (space-separated folder names), LEVELS (default "2500 5000 7500"),
#      OUT (scratch dir for databases and logs; default: a mktemp dir). Bun must be on PATH for the Bun server.
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PASSES="${1:-2}"
K6="${K6:-k6}"
read -r -a IMPLS <<< "${IMPLS:-cpp-epoll-khalefa-ow cpp-uwebsockets-tbsvttr c-fio-tbsvttr typescript-bun-khalefa-ow python-fastapi-cknutson12}"
read -r -a LEVELS <<< "${LEVELS:-2500 5000 7500}"
OUT="${OUT:-$(mktemp -d)}"
export JWT_SECRET=twelve-dollar-challenge HOST=127.0.0.1 PORT=3000
ulimit -n 65535
HZ=$(getconf CLK_TCK)
ticks() { awk '{print $14+$15}' "/proc/$1/stat" 2>/dev/null || echo 0; }

run_one() { # run_one <impl> <vus> <pass>
  local name=$1 vus=$2 pass=$3 run="$OUT/$1-$2-p$3"
  mkdir -p "$run"; cp "$ROOT/seed/feed.db" "$run/feed.db"; rm -f "$run"/feed.db-*
  SQLITE_PATH="$run/feed.db" setsid taskset -c 0 bash "$ROOT/submissions/$name/start.sh" > "$run/server.log" 2>&1 &
  local spid=$!
  for _ in $(seq 120); do curl -sf "http://$HOST:$PORT/health" >/dev/null && break; sleep 0.5; done
  ( peak=0; while kill -0 $spid 2>/dev/null; do r=$(ps -o rss= -p $spid | tr -d ' '); r=${r:-0}
      [ "$r" -gt "$peak" ] && peak=$r && echo $peak > "$run/peak_rss_kb"; sleep 1; done ) &
  local t0 c0 t1 c1 rc
  c0=$(ticks $spid); t0=$(date +%s.%N)
  (cd "$ROOT/bench" && taskset -c 1-7 "$K6" run -q --no-color -e VUS=$vus -e RAMP_UP=30s -e DURATION=60s \
      --summary-export "$run/summary.json" load.js > "$run/k6.log" 2>&1); rc=$?
  c1=$(ticks $spid); t1=$(date +%s.%N)
  kill -- -$spid 2>/dev/null; wait $spid 2>/dev/null; sleep 2
  local cpu; cpu=$(awk -v a=$c0 -v b=$c1 -v s=$t0 -v e=$t1 -v hz=$HZ 'BEGIN{printf "%.1f", 100*(b-a)/hz/(e-s)}')
  jq -r --arg n "$name" --arg v "$vus" --arg p "$pass" --arg rc "$rc" --arg cpu "$cpu" \
     --arg rss "$(cat "$run/peak_rss_kb" 2>/dev/null || echo 0)" '
    [$n, $v, $p, (if $rc=="0" then "yes" else "NO" end), (.metrics.http_reqs.count|tostring),
     (.metrics.http_req_duration.med*100|round/100|tostring), (.metrics.http_req_duration["p(95)"]*100|round/100|tostring),
     (.metrics.http_req_duration["p(99)"]*100|round/100|tostring), (.metrics.http_req_duration.max|round|tostring),
     ((.metrics.http_req_failed.value*10000|round/100)|tostring), $cpu, (($rss|tonumber)/1024|round|tostring)] | @tsv' \
    "$run/summary.json" 2>/dev/null || printf '%s\t%s\t%s\tNO-SUMMARY(rc=%s)\n' "$name" "$vus" "$pass" "$rc"
}

printf 'impl\tvus\tpass\tok\treqs\tp50\tp95\tp99\tmax\terr%%\tsrv_cpu%%\trss_mb\n'
k=0
for pass in $(seq "$PASSES"); do
  for vus in "${LEVELS[@]}"; do
    for i in "${!IMPLS[@]}"; do
      run_one "${IMPLS[$(( (i + k) % ${#IMPLS[@]} ))]}" "$vus" "$pass"
    done
    k=$((k + 1))
  done
done
