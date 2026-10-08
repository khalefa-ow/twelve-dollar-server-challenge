# C++17 + io_uring + SQLite group commit

**Direct serving on `0.0.0.0:80` by default**, with no Nginx required. Target: Ubuntu 24.04 x86_64, Linux 6.0 or newer,
one vCPU, 2 GiB RAM. All five challenge endpoints use the supplied database and schema.

| Component | Version |
|---|---|
| C++ | C++17, Ubuntu 24.04 GCC 13 toolchain |
| Networking | liburing **2.6**, checksum-pinned source, statically linked |
| Database | SQLite **3.53.4**, checksum-pinned amalgamation, compiled in |
| Crypto | Ubuntu 24.04 OpenSSL 3.0, `libcrypto` |
| HTTP / JSON | In-process parsers; no framework or JSON dependency |

From the repository root on Linux:

```bash
sudo bash submissions/cpp-iouring-khalefa-ow/install.sh
# Generate the seed first if seed/feed.db is absent:
bash seed/make-seed.sh
bash test/run.sh submissions/cpp-iouring-khalefa-ow

# Additional checks after the build; each check uses a fresh database copy:
scratch=$(mktemp -d)
cp seed/feed.db "$scratch/feed.db"
submissions/cpp-iouring-khalefa-ow/bin/core-test "$scratch/feed.db" "$PWD"
python3 submissions/cpp-iouring-khalefa-ow/tests/transport.py

# Optional direct-serving connection test:
ulimit -n 65535
python3 submissions/cpp-iouring-khalefa-ow/tests/transport.py --connections 15000
```

`install.sh` installs only the compiler, curl and OpenSSL headers. The repository test/seed tools
also require Node 18+, sqlite3, jq and openssl, as documented in the main README.
`build.sh` downloads and verifies the two pinned source archives, then builds `bin/server` and
`bin/core-test`. No prebuilt binaries are part of the submission.

For a standalone run, use a fresh database copy and the spec environment variables.
`HOST` defaults to `0.0.0.0` and `PORT` defaults to `80`:

```bash
cp seed/feed.db /tmp/iouring-feed.db
SQLITE_PATH=/tmp/iouring-feed.db JWT_SECRET=twelve-dollar-challenge \
  bash submissions/cpp-iouring-khalefa-ow/start.sh
```

Binding port 80 requires the challenge's `CAP_NET_BIND_SERVICE`. For an ordinary local run, use
`HOST=127.0.0.1 PORT=3000`. It also supports the challenge's Nginx configuration if desired.
The server stays in the foreground. SIGINT/SIGTERM drain socket operations and stop checkpointing.

## What reduces cost

- **One network/SQL thread.** `accept`, `recv`, and `send` use io_uring. SQLite's indexed queries
  execute in-process on this thread; SQLite uses its normal file VFS. A second thread only performs
  WAL checkpoints. `SINGLE_ISSUER` and `COOP_TASKRUN` match this architecture. No SQPOLL thread or
  CPU affinity is used.
- **Submit and reap in batches.** Multishot accept accepts many connections per submission.
  The loop consumes up to 256 completions per pass and submits newly queued network operations
  together. It handles partial sends and keeps response memory stable until the send completion.
- **Shared receive buffers.** A provided buffer ring contains 2,048 × 4 KiB buffers, **8 MiB total**,
  independent of the idle connection count. Idle sockets have no reserved receive buffer. Complete
  requests parse directly in the selected buffer; only partial or pipelined tails are copied.
- **Group commit.** Validated writes from one completion pass share `BEGIN IMMEDIATE` / `COMMIT`.
  Responses stay private until COMMIT succeeds. No timer delays a lone write to fill a batch.
  Statement or commit failure rolls back the whole group and returns 500 for its write requests.
  Same-socket pipelining pauses at writes until their commit, so a following read sees the write.
- **Persistent SQL statements**, including transaction statements. Feed and post queries use the
  supplied indexes; likes use `INSERT ... SELECT ... WHERE EXISTS ... ON CONFLICT DO NOTHING`.
  Duplicate likes see earlier inserts in their group and still produce exactly one 201.
- **An 8 MiB SQLite page cache plus 1 GiB mmap limit.** This avoids a large page-cache bookkeeping
  cost while allowing SQLite to read through the OS page cache. The mmap size is a mapping limit,
  not an allocation of 1 GiB. No application data or query results are cached across requests.
- **WAL with `synchronous=NORMAL`**, as allowed by rule 6. Background PASSIVE checkpoints run every
  200 ms, with SQLite's 1,000-page auto-checkpoint retained to bound sustained WAL growth.
- **HS256 on every authenticated request.** Only the HMAC key's initial hash states are reused;
  each token's signature, algorithm, expiration and payload are verified again.
- **Reused buffers and compact JSON.** No general-purpose HTTP framework, thread-pool handoff,
  per-request SQL preparation, or response serialization tree.

Connections idle for over 120 seconds are closed, meeting the 65-second Nginx keep-alive minimum.
The server handles chunked bodies, fragmented requests, pipelining, and disconnects with operations
outstanding. Ambiguous Content-Length/Transfer-Encoding framing is rejected. Headers and bodies
are limited to 16 KiB each; bodies above that size are outside the challenge spec.

The JSON and authentication helpers were adapted from this repository's MIT-licensed
`cpp-epoll-khalefa-ow` submission. Unicode validation and JavaScript whitespace trimming are kept.

## Validation and further tuning

**Local validation:** 128 portable core checks passed on macOS with AddressSanitizer and
UndefinedBehaviorSanitizer, using the pinned SQLite 3.53.4 source. This covers exact golden reads,
JSON/JWT validation, HTTP framing, independent-reader commit visibility, duplicate likes, rollback,
injected COMMIT failure, and recovery. **The Linux server has not yet been compiled or run.**
Run the official suite and `tests/transport.py` on Linux before treating this as benchmark-ready.
The transport test checks success responses against a separate database connection and exercises
concurrency, slow readers, pipeline ordering, buffer lifetimes, and shutdown.

No throughput win is claimed without the challenge benchmark. Measure the read-heavy workload,
not just insert throughput: writes are approximately 8% of its requests. For comparison with the
existing epoll implementation, the repository's CPU-cost tool accepts both folder names:

```bash
IMPLS='cpp-epoll-khalefa-ow cpp-iouring-khalefa-ow' bash comparison/cpu-cost.sh
```

For another reduction, profile these candidates on the one-core machine:

1. Register sockets as fixed files to reduce per-operation fd lookup/reference overhead.
2. Group the read queries from a completion pass into one short SQLite read transaction to reduce
   WAL read-lock churn. Each request must still execute its own SQL and the snapshot must end
   before the write transaction starts.
3. Compare 2/8/32 MiB SQLite caches and 64/256/512-completion passes. A larger group is not
   automatically faster for this mostly-read workload.
4. If query time dominates, inspect the feed's live `count(*)` operations on popular posts.
   The fixed schema and prohibition on result caching constrain improvements here.

Multishot receive and zero-copy send are also possibilities, but need measured gains to justify
their extra buffer ownership and backpressure complexity for these small responses.

liburing's [provided buffer documentation](https://github.com/axboe/liburing/blob/liburing-2.6/man/io_uring_register_buf_ring.3)
describes sharing buffers across more pending receives than there are buffers.
SQLite's [WAL documentation](https://www.sqlite.org/wal.html) describes checkpointing and NORMAL
versus FULL synchronization. NORMAL meets this challenge's rule; FULL adds a WAL sync per commit
if power-loss durability is needed for another deployment.

MIT, under the repository's [license](../../LICENSE).
