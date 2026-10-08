# khalefa-ow C++ submissions: epoll vs io_uring (2026-10-08)

A comparison of five C++ servers by khalefa-ow, run on one laptop under identical conditions.

| Folder | What it is |
|---|---|
| `cpp-epoll-khalefa-ow` | v1: hand-written epoll loop, SQLite |
| `cpp-epoll_inmem-khalefa-ow` | v2 of the above: exclusive locking, tuned for tail latency and kernel time |
| `cpp-epoll-memstore-khalefa-ow` | same HTTP code, custom in-memory store, **no SQLite** (not a valid entry) |
| `cpp-iouring-khalefa-ow` | new: io_uring transport, SQLite group commit |
| `cpp-iouring-top20-khalefa-ow` | new: `cpp-iouring` plus a cached `/feed` response, cleared on every committed write |

All five pass `test/test.sh` (42/42). These are not official scores; see [Caveats](#caveats).

## Summary

| Server | CPU per request (mix) | k6 p99 at 2,500 / 5,000 / 7,500 VUs | Valid under the rules? |
|---|---|---|---|
| cpp-epoll | 43–48 µs | 201 † / 4.1 / 193 † ms | yes |
| **cpp-epoll_inmem** | **36 µs** | 3.5 / 5.7 / 8.2 ms | **yes: best valid entry** |
| cpp-iouring | 40–46 µs | 7.2 / 4.4 / 3.0 ms | yes, but intermittently fails to start (ENOMEM) |
| cpp-iouring-top20 | 30–34 µs | 5.4 / 11.1 / 3.4 ms | **no**: response cache (rule 5) |
| cpp-epoll-memstore | about 20 µs | 2.5 / 2.2 / 2.2 ms | no: not SQLite (rule 2) |

† Load-generator stalls, not the server; see the k6 section.

- **io_uring alone doesn't lower CPU cost.** `cpp-iouring` costs about the same per request as
  `cpp-epoll` and slightly more than `cpp-epoll_inmem`. Its feed (57–71 µs) is more expensive than
  inmem's (about 50 µs), and its writes are too. Its 7,500 VU p99 was the best of the SQLite
  servers, but with one pass and 3–8 ms between them, that's within noise.
- **top20's whole advantage is the feed cache.** It is the cheapest SQLite server on the mix only
  because a cached feed costs 19–36 µs instead of about 50–60 µs. Rule 5 bans response caches
  ("no response caches, query-result caches, …"), and clearing the cache on every write doesn't
  change that. Its writes cost slightly more than inmem's.
- **memstore is the floor:** about 20 µs. For inmem, SQLite therefore accounts for roughly 15 µs of
  its 36 µs per request.

## Startup failures in both io_uring servers

`io_uring_setup` (SQ 2,048 and CQ 65,536 entries, `SINGLE_ISSUER | COOP_TASKRUN | CQSIZE`) often
returned `ENOMEM` at startup. One server needed up to 6 attempts, and a whole 6-attempt slot once
failed. A standalone probe with the same parameters then succeeded repeatedly, even while a server
held a ring. The usual pattern was a failed first attempt and a successful retry, which fits a
large kernel allocation failing under memory fragmentation. No server was ever left without a
ring at runtime; the failures were all at startup. On a 2 GiB droplet with no retry, the server
could simply fail to start. A smaller CQ, or retrying with smaller sizes, would avoid that.

## CPU cost per request (`comparison/cpu-cost.sh`)

Server CPU time per request in µs, user plus kernel, for 2 passes (pass 1 / pass 2). **random**
draws post ids from 1..500,000, as in the original script. **hot** (`HOT=1`) draws post and like
ids from the current `/feed`. Lower is better.

| Server | mode | health | post | feed | like | create | **mix** |
|---|---|---|---|---|---|---|---|
| cpp-epoll | random | 18.8 / 14.8 | 29.9 / 26.2 | 57.9 / 52.6 | 52.6 / 48.8 | 40.8 / 38.8 | **49.2 / 37.6** |
| | hot | 17.2 / 24.6 | 29.1 / 34.1 | 61.6 / 79.2 | 39.1 / 49.5 | 57.0 / 46.8 | **49.4 / 45.9** |
| cpp-epoll_inmem | random | 17.4 / 14.7 | 25.4 / 23.3 | 50.3 / 48.3 | 41.7 / 40.5 | 34.4 / 31.8 | **37.7 / 34.0** |
| | hot | 16.5 / 16.3 | 24.8 / 23.5 | 55.5 / 50.1 | 29.1 / 31.4 | 27.1 / 26.5 | **34.4 / 38.4** |
| cpp-iouring | random | 17.9 / 17.0 | 31.0 / 36.8 | 57.6 / 61.0 | 48.1 / 49.6 | 45.3 / 41.9 | **40.3 / 52.6** |
| | hot | 25.9 / 21.1 | 32.2 / 26.6 | 71.3 / 63.0 | 36.3 / 45.7 | 39.9 / 111.0 ‡ | **40.4 / 200.7 ‡** |
| cpp-iouring-top20 | random | 21.4 / — | 30.3 / — | 19.4 / — | 47.4 / — | 44.5 / — | **30.5 / —** |
| | hot | 26.3 / 96.9 ‡ | 42.9 / 67.9 | 22.8 / 36.1 | 115.2 ‡ / 50.3 | 52.0 / 60.3 | **31.9 / 36.6** |
| cpp-epoll-memstore | random | 15.9 / 15.6 | 16.6 / 16.8 | 17.6 / 22.3 | 19.9 / 20.5 | 25.6 / 21.3 | **20.1 / 17.4** |
| | hot | 20.8 / 17.9 | 20.0 / 23.5 | 18.3 / 22.1 | 27.3 / 24.8 | 25.7 / 41.9 | **18.3 / 23.2** |

‡ Outlier: the machine was swapping (about 74 MB free), and the cpp-iouring hot pass 2 mix ran at
only 1,691 requests/s. "—" means the server failed all 6 start attempts (ENOMEM). An earlier session
the same day measured an older top20 build that dropped old posts, so its results aren't comparable
and aren't included.

Individual results vary by 10–25% between passes, so differences under about 5 µs on the mix
aren't meaningful.

## k6 load test (`comparison/ladder.sh`, 1 pass)

| Server | VUs | p50 | p95 | p99 | max | err% | server CPU | peak RSS |
|---|---|---|---|---|---|---|---|---|
| cpp-epoll | 2,500 | 0.46 | 5.76 | 200.6 | 910 | 0 | 4.8% | 13 MB |
| | 5,000 | 0.33 | 0.92 | 4.06 | 43 | 0 | 5.1% | 20 MB |
| | 7,500 | 0.33 | 1.20 | 192.7 † | 74,663 † | 0.41 † | 11.1% | 23 MB |
| cpp-epoll_inmem | 2,500 | 0.37 | 0.67 | 3.46 | 214 | 0 | 2.7% | 215 MB * |
| | 5,000 | 0.35 | 0.73 | 5.68 | 127 | 0 | 5.2% | 222 MB * |
| | 7,500 | 0.35 | 0.83 | 8.18 | 167 | 0 | 7.1% | 224 MB * |
| cpp-iouring | 2,500 | 0.47 | 2.16 | 7.15 | 156 | 0 | 4.0% | 39 MB |
| | 5,000 | 0.38 | 0.83 | 4.42 | 332 | 0 | 5.8% | 58 MB |
| | 7,500 | 0.36 | 0.76 | 3.04 | 77 | 0 | 7.6% | 78 MB |
| cpp-iouring-top20 | 2,500 | 0.34 | 0.72 | 5.38 | 290 | 0 | 2.5% | 40 MB |
| | 5,000 | 0.31 | 0.81 | 11.12 | 320 | 0 | 4.9% | 61 MB |
| | 7,500 | 0.32 | 0.71 | 3.42 | 119 | 0 | 6.3% | 77 MB |
| cpp-epoll-memstore | 2,500 | 0.32 | 0.57 | 2.50 | 153 | 0 | 1.8% | 279 MB |
| | 5,000 | 0.28 | 0.52 | 2.24 | 24 | 0 | 3.1% | 279 MB |
| | 7,500 | 0.27 | 0.50 | 2.23 | 28 | 0 | 4.2% | 277 MB |

Latencies are in ms. All 15 runs passed the scoring thresholds.

† `cpp-epoll` at 7,500 VUs completed only 45,897 of about 78,000 requests, with a 75-second maximum,
while the machine's swap was full. The load generator stalled, not the server, which used 11% of
its core. Its 200 ms p99 at 2,500 VUs is the same kind of one-off spike seen in
[`RESULTS.md`](RESULTS.md).

\* inmem's RSS includes the pages of `feed.db` mapped by `PRAGMA mmap_size = 1 GiB`. Those are
file-backed pages the kernel can reclaim, not heap.

## Caveats

- **Machine:** WSL2 on an 11th Gen Intel Core i5-1145G7 laptop, not the benchmark VM. Each server was
  pinned to core 0 and the load generator ran on cores 1–7.
- **Memory pressure:** about 2–3 GB was free, and swap filled up at times. This adds noise to every
  number and caused the outliers marked ‡ and †.
- **Short runs:** one k6 pass (30 s ramp, 60 s hold) and two CPU passes per server.
- **Port:** servers listened on `127.0.0.1:3000`, because this machine doesn't let a normal user
  bind port 80. They all default to `0.0.0.0:80`.
- **Build:** `cpp-iouring` was built with `gcc` pointing at `gcc-14`. On this host, `gcc` is GCC 11
  and `g++` is GCC 14, which breaks its link-time optimization (LTO). `cpp-iouring-top20` now
  builds without LTO.

Raw data: [`khalefa-ow-2026-10-08-cpu.txt`](khalefa-ow-2026-10-08-cpu.txt) and
[`khalefa-ow-2026-10-08-ladder.tsv`](khalefa-ow-2026-10-08-ladder.tsv). The ladder was run through a
copy of `ladder.sh` that retries server startup; no retry was needed in this run.
