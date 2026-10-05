# C++ + epoll + app-specific in-memory store (experiment)

> **Not a valid entry.** This breaks rule 2 (the database is SQLite) and rule 5 (no in-memory
> copies of tables). It lives outside `submissions/` on purpose. It answers one question: how much
> of the per-request cost of [`cpp-epoll_inmem-khalefa-ow`](https://github.com/khalefa-ow/twelve-dollar-server-challenge/tree/cpp-epoll_inmem-khalefa-ow/submissions/cpp-epoll_inmem-khalefa-ow)
> is SQLite?

The HTTP server, JWT check and JSON parser are the same code as the v2 SQLite server. Only the data
layer changed: SQLite is replaced by a store that holds the data in memory, laid out for these five
endpoints, and makes every write durable through its own WAL.

## Running it

```bash
bash build.sh    # bin/server (no SQLite) and bin/import (feed.db -> snapshot)
SQLITE_PATH=... JWT_SECRET=... HOST=... PORT=... bash start.sh
bash test/run.sh experiments/cpp-epoll-memstore   # from the repo root: 42/42
```

`start.sh` runs `bin/import` first. It converts `feed.db` into the store's snapshot (about 2 s), or
does nothing if the store already came from this same `feed.db`. Then it starts the server, which
never opens SQLite.

| Env | Default | |
|---|---|---|
| `STORE_DIR` | `$SQLITE_PATH.memstore` | snapshot and WAL files |
| `WAL_SYNC` | `normal` | Like SQLite's WAL + `synchronous=NORMAL`. `full`: `fdatasync` every batch that wrote, like `synchronous=FULL` |
| `SNAPSHOT_WAL_MB` | `64` | WAL size that triggers a snapshot |

## Memory layout ([`src/store.hpp`](src/store.hpp))

- **Posts: an array indexed by id.** Each entry holds the post's JSON already rendered up to
  `"like_count":`, plus the like count. `GET /posts/:id` is an array lookup and two appends.
- **Feed: post ids sorted by `(created_at, id)`.** The feed is the last 20. A new post is almost
  always the newest, so adding one is a `push_back`.
- **Likes: an open-addressing hash set.** Keys are `user << 32 | post` in 8 bytes, with linear
  probing at most 70% full. It only answers "already liked?"; the count lives on the post.
- **Authors:** usernames by user id. A post whose author isn't in `users` is never served, as the
  SQLite version's `JOIN` behaves.

## Durability

- **WAL**: every post and like is appended as a record: `{u32 length, u32 CRC-32C, payload}`.
  Each event-loop batch costs one `write()`, and **no response of a batch is sent until its records
  are written**. A batch with no writes skips the syscall.
- **Same guarantees as SQLite's WAL mode with `synchronous=NORMAL`** (the default):
  - A commit survives a process crash at once. It reaches the disk with the OS's writeback, so a
    power loss can drop the last commits but never corrupts the store.
  - As SQLite syncs the WAL before a checkpoint, the server `fdatasync`s a WAL file before a
    snapshot takes it over, and syncs replayed files at startup. Whatever survives a power loss is
    a prefix of the commits.
  - `WAL_SYNC=full` also `fdatasync`s every batch that wrote, like `synchronous=FULL`. Every batch
    then waits for the disk: 2 ms p50 and 10 ms p99 per sync on the test machine, which showed up
    directly in k6's p95/p99.
- **Recovery**: the server maps the snapshot, replays the WAL files that come after it, and cuts
  off a torn tail: a cut-short record or a CRC mismatch was never acknowledged. Startup takes
  about 0.2–0.5 s.
- **Snapshots** (they compact the WAL): once the WAL reaches `SNAPSHOT_WAL_MB`, the server starts
  a new WAL file and `fork()`s.
  - The child closes the sockets it inherited and writes the frozen copy-on-write image to
    `snapshot.tmp`. Then it runs `fsync`, `rename` and `fsync(dir)`.
  - The parent keeps serving. When the child exits cleanly, the parent deletes the WAL files the
    snapshot covers.
  - Rendered JSON for snapshot posts is served straight from the mapped snapshot file.
- Tested: `kill -9` of the whole process group with writes in the WAL, with a snapshot in
  progress, and after a snapshot finished, plus a torn record appended to the WAL. Each time the
  feed and the like counts after restart were identical.

## Cost per request vs. the SQLite v2 server

`comparison/cpu-cost.sh`, same machine, server pinned to one core. The client tops out at about
12k req/s, so these are CPU costs, not maximum throughput.

| endpoint | SQLite v2 user µs | memstore user µs | SQLite v2 total µs | memstore total µs |
|---|---|---|---|---|
| health | 2.0 | 1.6 | 15.2 | 16.8 |
| post | 10.1 | 3.0 | 25.3 | 17.4 |
| feed | 47.0 | 2.8 | 65.6 | 18.8 |
| like | 35.0 | 4.5 | 80.1 | 18.5 |
| create | 15.2 | 5.5 | 35.8 | 21.4 |
| mix | 21.0 | 2.4 | 36.6 | 12.3 |

These numbers were measured with `WAL_SYNC=full`. With the store out of the way, what remains is
almost all kernel networking time (`sys`, about 15 µs per request). Snapshot children are separate
processes, so their CPU doesn't show in these numbers (no snapshot ran during the 8 s windows).

Memory: about 112 MB anonymous (post array, like set, new posts) plus the 174 MB snapshot mapping,
which the kernel can reclaim.

## Limits

- `likes.created_at` isn't stored, since no endpoint returns it.
- With `WAL_SYNC=full`, each `fdatasync` blocks the single thread. A dedicated sync thread would
  let reads continue during it, but the batch's responses would still have to wait.
