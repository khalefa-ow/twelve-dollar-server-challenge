# C++17 + io_uring + a 20-post window with WAL

Experimental variant with **no SQLite dependency**. It keeps the latest 20 posts in RAM,
per-user like membership, precomputed like counts, and cached post/feed JSON. A custom
write-ahead log preserves that state across restarts. It serves directly on **`0.0.0.0:80`**
by default, using the io_uring transport from `cpp-iouring-khalefa-ow`.

New posts evict the oldest slot when the window is full. The evicted post and its memberships
leave live state; `GET /posts/:id` and `POST /posts/:id/like` subsequently return 404. This
intentionally changes the challenge's SQLite/full-history requirements.

From the repository root on Ubuntu 24.04 / Linux 6.0+:

```bash
sudo bash submissions/cpp-iouring-top20-khalefa-ow/install.sh
bash submissions/cpp-iouring-top20-khalefa-ow/build.sh

# Optional, before initializing the store: seed from the generator without a database.
bash submissions/cpp-iouring-top20-khalefa-ow/prepare-seed.sh

# Allow port 80 as a normal user; repeat after rebuilding the binary.
sudo setcap cap_net_bind_service=+ep submissions/cpp-iouring-top20-khalefa-ow/bin/server
JWT_SECRET=twelve-dollar-challenge bash submissions/cpp-iouring-top20-khalefa-ow/start.sh
```

`build.sh` downloads SHA-256-verified liburing **2.6** source and links it statically. The other
linked dependency is OpenSSL `libcrypto`. The build uses GCC/C++17 and `-O3 -march=native -flto`;
compile on the target machine. No database library is downloaded, included or linked.
`prepare-seed.sh` uses Node 18+ and Python 3, streams generated TSVs into a 20-post JSON seed,
saves benchmark tokens, and removes the temporary TSVs.

| Environment variable | Default | Meaning |
|---|---|---|
| `JWT_SECRET` | Required | HS256 key; match the key used for benchmark tokens |
| `HOST` | `0.0.0.0` | Listen address |
| `PORT` | `80` | Listen port |
| `STORE_DIR` | This variant's `data/` through `start.sh`; `./data` for the binary | Persistent snapshot, WAL, and writer lock |
| `TOP20_SEED` | `bin/top20-seed.json` if present through `start.sh` | Initial state, used only when no snapshot exists |

Restart with the same `STORE_DIR` to recover posts, memberships, counts and the next id.
For a fresh empty experiment, use a new `STORE_DIR` and `TOP20_SEED=""`. Changing the seed
has no effect on an initialized store. For local testing without the port capability, use
`HOST=127.0.0.1 PORT=8080`. SIGINT/SIGTERM drains socket operations and outstanding write groups.

## Group commit and recovery

1. Valid mutations pause their connections and enter a bounded queue. Up to **256 mutations**
   observed in a completion batch share one commit; arriving writes accumulate while that
   commit is in flight. There is no deliberate batching sleep.
2. A candidate state shares untouched posts with committed state. A like clones only the
   touched post's bitmap; creates replace slots. Reads continue to use committed state.
3. The server appends one binary frame using **`io_uring_prep_write`**, handling partial writes,
   then submits **`io_uring_prep_fsync(..., IORING_FSYNC_DATASYNC)`** after the write CQE.
4. Only a successful sync CQE publishes the candidate and releases its responses. A pipeline
   resumes after its write commits, so its following GET sees that mutation. Duplicate-only
   or missing-post groups need no log append or sync.

Frames contain a monotonic group sequence, a checksummed header, CRC32C-protected operations,
and a commit marker. Creates record the post id, timestamp and JSON prefix; likes record the
post id, user id and resulting precomputed count. Recovery validates and applies whole frames.
An incomplete final frame is truncated before new writes. Corrupt complete frames, invalid
counts and sequence gaps stop startup. A write/sync error stops the server without acknowledging
or publishing that group.

After **64 MiB** of WAL, a checkpoint stores just the 20 retained posts, next id, bitmap/hash-set
memberships and precomputed counts. It syncs a temporary snapshot, atomically renames it,
syncs the directory, then truncates and syncs the WAL. Recovery skips frames already covered
by the snapshot, including a crash between rename and truncation. Initialization also syncs
new directory entries. An exclusive file lock allows one writer per store. Checkpoints are
small synchronous operations on the event thread; normal WAL appends and syncs are asynchronous.
Historical post data may remain in the WAL until the next checkpoint.

The sync flag maps to `fdatasync` in [liburing's fsync API](https://raw.githubusercontent.com/axboe/liburing/liburing-2.6/man/io_uring_prep_fsync.3).
The file/directory ordering follows the [Linux fsync documentation](https://man7.org/linux/man-pages/man2/fsync.2.html)
and [atomic rename semantics](https://man7.org/linux/man-pages/man2/rename.2.html).
A crash can recover a complete frame whose reply never reached the client. Like retries remain
idempotent; post creation has no request idempotency key.

## Read and like cost

- A **20-slot ring** bounds lookup to 20 ids. There is no user or historical post table.
- Each retained post uses an **8 KiB bitmap** for user ids 1–65,536, covering all challenge users.
  All 20 committed bitmaps total **160 KiB**. Higher positive 64-bit ids use a per-post hash set.
- A first like sets one bit/entry and increments a stored counter. A repeat leaves it unchanged.
  Reads use the counter and cached JSON; they never count membership bits or query the log.
  Recovery validates snapshot counters against membership once during startup.
- Copy-on-write staging keeps committed and pending membership/counts separate. Untouched
  slots remain shared; only the live 20-slot windows are retained in RAM.
- One thread serves requests and submits network/disk IO. Multishot accept, batched completion
  handling and an **8 MiB shared receive pool** keep idle sockets from requiring read buffers.
  Send buffers and pending connections remain alive until their operations/commits finish.
- Every authenticated request still verifies HS256, expiration and payload. Strict JSON, UTF-8,
  whitespace trimming, body length and HTTP framing checks remain in place.

Routes are `/health`, `/feed`, `/posts/:id`, `/posts`, and `/posts/:id/like`. Health reports
`{"status":"ok","store":"memory-wal","uptime_s":...}`. Empty feeds have zero posts; after 20
inserts they have exactly 20, newest insertion first. Seed loading orders the initial posts by
`(created_at,id)`. New timestamps use real UTC milliseconds; ids are signed 64-bit integers.

## Seed format

Seeds contain at most 20 posts, exact per-user memberships and optional precomputed counts:

```json
{"next_id":2,"posts":[{"id":1,"body":"hello","created_at":"2025-12-31T23:59:59.000Z","author":"user","liked_user_ids":[1,2],"like_count":2}]}
```

`tools/export_seed.py <tsv-directory> <output.json>` selects the newest 20 posts by timestamp/id,
keeps their author names and memberships, and precomputes their counts. `next_id` uses the
maximum id across **all** input posts, including evicted ones. Memberships are deduplicated.
If a count is supplied, it must match the distinct membership count. Invalid seeds stop startup.
The initial seed is durably saved in the snapshot, so later recovery needs no seed file.

## Validation and benchmarking

**Local checks:** 2,172 production-core checks and 767 WAL/batch checks passed on macOS under
AddressSanitizer and UndefinedBehaviorSanitizer. The seed feed matches `test/golden/feed.json`
byte for byte (20 posts, 87 likes). Seed-converter/loader checks also passed. The WAL checks
exercise restart, per-user recovery, committed-read isolation, duplicate groups, eviction,
every partial prefix of a frame, corruption rejection, checkpoints and process exit without
cleanup. **The actual io_uring transport still needs compilation and execution on Linux.**

After building on Linux:

```bash
submissions/cpp-iouring-top20-khalefa-ow/bin/core-test
submissions/cpp-iouring-top20-khalefa-ow/bin/wal-test
python3 submissions/cpp-iouring-top20-khalefa-ow/tests/seed.py

# If prepare-seed.sh was run:
submissions/cpp-iouring-top20-khalefa-ow/bin/core-test \
  submissions/cpp-iouring-top20-khalefa-ow/bin/top20-seed.json test/golden/feed.json

# Starts servers on an ephemeral port with a temporary persistent store:
python3 submissions/cpp-iouring-top20-khalefa-ow/tests/transport.py
ulimit -n 65535
python3 submissions/cpp-iouring-top20-khalefa-ow/tests/transport.py --connections 15000
```

Transport checks cover concurrent writes, duplicate/distinct-user likes, eviction, pipeline
ordering, fragmented/chunked bodies, slow readers, disconnects, SIGKILL recovery, graceful
restart and a torn WAL header. Native core/WAL tests also build with `clang++ -std=c++17` and
`-lcrypto` on macOS when OpenSSL include/library paths are supplied.

For the existing k6 workload, using generated tokens and a fresh store:

```bash
k6 run -e BASE_URL=http://127.0.0.1:80 -e VUS=2500 \
  -e TOKENS="$PWD/submissions/cpp-iouring-top20-khalefa-ow/bin/tokens.json" bench/load.js
```

The original test suite expects historical posts, which this window discards. The k6 client may
also receive 404 when its selected post is evicted during think time. Interpret that error rate
alongside throughput and latency; no throughput result is claimed yet.

Source helpers and transport adapted from this repository's MIT-licensed C++ io_uring submission.
MIT, under the repository's [license](../../LICENSE).
