# Local comparison of five submissions

Five submissions to the $12 Server Challenge, run on one laptop under identical conditions:

| Folder | Stack | Author |
|---|---|---|
| `cpp-epoll-khalefa-ow` | C++17, hand-written epoll loop, SQLite 3.53.4 compiled in | khalefa-ow |
| `cpp-uwebsockets-tbsvttr` | C++20, uWebSockets/uSockets, SQLite 3.53.4 | tbsvttr |
| `c-fio-tbsvttr` | C, facil.io (`fio-stl.h`), SQLite 3.53.4 | tbsvttr |
| `typescript-bun-khalefa-ow` | Bun 1.4.2, `Bun.serve` + `bun:sqlite` | khalefa-ow |
| `python-fastapi-cknutson12` | Python, FastAPI + uvicorn (uvloop, httptools), stdlib `sqlite3` | cknutson12 |

All five pass `test/test.sh` (42/42), and all five serve directly without Nginx.

**These are not official scores.** The official benchmark uses a 1-vCPU DigitalOcean droplet with
k6 on a separate machine and 5-minute holds. The numbers below are useful for comparing the five
servers against each other, not for predicting their official scores.

## Summary

At up to 7,500 virtual users (VUs):

- **The two C++ servers are tied for best.** Neither failed a run, both use about 7–9% of one core
  and about 26 MB, and their tail latencies trade places from run to run.
  `cpp-epoll` had the lowest worst-case p99 (15.7 ms across all six runs).
- **c-fio** is close behind on latency, but uses about 1.4× the CPU and about 2.7× the memory.
- **Bun** uses about 1.5–2.5× the CPU of the C++ servers and about 65 MB.
- **Python** uses 4–6× the CPU of the C++ servers and about 150 MB. It is the only server near its
  limits at 7,500 VUs.
- **c-fio, Bun and Python each had at least one large latency spike** (p99 of 400 ms or more).
  Each spike appeared in only one of that server's two passes, sometimes while the server used under
  10% of its core (see [Caveats](#caveats)), so they are most likely caused by the test machine.

## Load test (k6, `bench/load.js`)

Two passes over 2,500 / 5,000 / 7,500 VUs, 30 runs in total, with 0 request errors in every run.
Each cell shows **pass 1 / pass 2**. **Server CPU** is the server process's own CPU time divided by
the run's wall time, so 100% is one full core.

### p99 latency (scoring limit: under 1,000 ms)

| Server | 2,500 VUs | 5,000 VUs | 7,500 VUs | Runs failed |
|---|---|---|---|---|
| cpp-epoll | 2.7 / 2.7 ms | 2.8 / 11.2 ms | 15.7 / 2.4 ms | **0 / 6** |
| cpp-uwebsockets | 1.9 / 1.6 ms | 3.1 / 6.3 ms | 44.8 / 1.8 ms | **0 / 6** |
| c-fio | 5.1 / 1.9 ms | 4.0 / **2,630 ms** | 433 / 57 ms | 1 / 6 |
| Bun | 2.6 / 18.0 ms | 997 / 1.0 ms | 412 / 973 ms | 0 / 6 |
| Python | 13.5 / 5.3 ms | 506 / 27 ms | **4,446** / 12 ms | 1 / 6 |

Bun's 997 ms at 5,000 VUs passed, but only just. **Bold** marks a run that failed a threshold.

### p95 latency (scoring limit: under 500 ms)

| Server | 2,500 VUs | 5,000 VUs | 7,500 VUs |
|---|---|---|---|
| cpp-epoll | 0.64 / 0.65 ms | 0.63 / 0.90 ms | 1.94 / 0.62 ms |
| cpp-uwebsockets | 0.56 / 0.62 ms | 0.65 / 0.81 ms | 1.99 / 0.57 ms |
| c-fio | 0.75 / 0.75 ms | 0.74 / **919 ms** | 6.3 / 1.5 ms |
| Bun | 0.73 / 1.07 ms | 405 / 0.67 ms | 103 / 0.82 ms |
| Python | 2.95 / 1.88 ms | 131 / 5.1 ms | 146 / 2.5 ms |

### Server CPU and peak memory

| Server | CPU at 2,500 VUs | CPU at 5,000 VUs | CPU at 7,500 VUs | Peak RSS at 7,500 VUs |
|---|---|---|---|---|
| cpp-epoll | 2.8 / 2.9% | 4.9 / 5.9% | 8.4 / 6.6% | 26 MB |
| cpp-uwebsockets | 2.5 / 2.8% | 4.9 / 5.4% | 9.0 / 6.5% | 27–28 MB |
| c-fio | 3.7 / 3.7% | 6.4 / 9.1% | 12.4 / 10.3% | 71–72 MB |
| Bun | 4.9 / 6.4% | 18.3 / 7.6% | 21.2 / 9.9% | 65–67 MB |
| Python | 16.9 / 14.1% | 31.9 / 29.6% | 53.9 / 29.0% | 151–152 MB |

The p50 latency was 0.3–0.7 ms for every compiled server and Bun at every level, and 0.7–1.9 ms
for Python. Raw rows, including p50, max and request counts, are in
[`ladder-results.tsv`](ladder-results.tsv).

## CPU cost per request

Each server ran on one core and received one request type at a time for 8 seconds, from 200
keep-alive connections. The table shows the server's CPU time per request in µs, user plus kernel.
**mix** uses `bench/load.js`'s ratios: 1 feed, 1 single post, 0.15 likes and 0.02 new posts per
loop. Lower is better.

| Server | health | `GET /posts/:id` | `GET /feed` | like | create | **mix** |
|---|---|---|---|---|---|---|
| cpp-epoll | **15.6** | **31.0** | **56.3** | **100.6** | 106.3 | 82.4 * |
| cpp-uwebsockets | 21.3 | 39.6 | 59.7 | 269.7 | 136.2 | **48.1** |
| c-fio | 32.5 | 42.3 | 70.2 | 117.8 | **93.4** | 53.7 |
| Bun | 38.6 | 50.5 | 62.3 | 135.5 | 131.4 | 56.5 |
| Python | 134.5 | 219.7 | 226.7 | 371.2 | 341.1 | 241.5 |

\* This value is probably an outlier. Earlier runs of the same `cpp-epoll` binary measured
42–58 µs for the mix, and its own per-endpoint costs predict about 45 µs.

Notes on these numbers:

- **Writes vary a lot between runs.** For example, `cpp-epoll`'s like cost measured 55 µs in one
  earlier session and 100 µs here. Writes are only about 8% of the benchmark's requests.
- **The kernel accounts for 30–60% of the cost** on the compiled servers (accept, `recv`, `send`,
  epoll, WAL writes). It is roughly the same for all of them, so the differences are mostly in user
  space.
- **One hotspot was found in `cpp-epoll` with `perf`:** 26% of a like's user CPU was SQLite's
  `pcache1TruncateUnsafe`, which scans the whole page-cache hash table on every commit when
  `cache_size` is large (32 MiB here). A 200-page cache removes the hotspot, but the overall gain
  was about 5%, within run-to-run noise, so the submission keeps 32 MiB.
- **The load generator (Node) tops out at 10–15k requests/s.** These figures are CPU costs per
  request, not maximum throughput.

Raw output is in [`per-request-cpu.txt`](per-request-cpu.txt).

## Caveats

- **The test machine is not the benchmark VM.** It is an 11th Gen Intel Core i5-1145G7 laptop
  (8 logical cores, 7 GB RAM) running WSL2 (Ubuntu 22.04, kernel 6.18). Each server was pinned to
  core 0 to stand in for the single vCPU, and k6 ran on cores 1–7 of the same machine. A droplet's
  vCPU is probably 2–3× slower than this core.
- **The runs were short:** a 30 s ramp and a 60 s hold, against the official 60 s ramp and 5-minute
  hold. Each level was run only twice.
- **7,500 VUs was the ceiling here.** k6 needs about 0.34 MB per VU, so this machine could not push
  any server to its limit. The compiled servers stayed under about 12% CPU at that level.
- **Large latency spikes come from the test environment.** A p99 of hundreds or thousands of ms
  appeared at random and hit each affected server in only one of its two passes. The clearest case
  is c-fio failing at 5,000 VUs (p95 919 ms) while using only 9% of its core, when its other pass at
  that level had a p99 of 1.9 ms. An earlier `cpp-epoll` run outside this data set stalled for about
  a minute in the same way, and two monitored reruns did not reproduce it.
- **Build differences:**
  - `cpp-uwebsockets` was built with clang 14 against GCC 11's libstdc++ headers. clang 14 cannot
    parse this machine's GCC 14 headers, so a local-only compiler wrapper was needed. On Ubuntu
    24.04 the submission builds as written.
  - Python ran on 3.10 here, while its README targets Ubuntu 24.04's 3.12.

## Reproducing

From the repo root, with `seed/feed.db` built (`bash seed/make-seed.sh`), each submission's
`build.sh` run, and Node ≥ 18, `jq`, `curl` and `ss` installed:

```bash
# k6 ladder: 2 passes over 2,500 / 5,000 / 7,500 VUs (about 70 minutes)
K6=/path/to/k6 PATH=/path/to/bun-dir:$PATH bash comparison/ladder.sh 2 > comparison/ladder-results.tsv

# CPU cost per request (about 5 minutes)
PATH=/path/to/bun-dir:$PATH bash comparison/cpu-cost.sh > comparison/per-request-cpu.txt
```

The tools used were k6 v2.3.0, Bun 1.4.2 and Node 20.20.0.
