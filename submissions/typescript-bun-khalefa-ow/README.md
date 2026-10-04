# TypeScript + Bun

| | |
|---|---|
| Language | TypeScript, run directly by Bun (no build step) |
| Runtime / framework | **Bun 1.4.2** (`Bun.serve` with its built-in `routes`), installed from the official release zip (sha256-pinned) |
| SQLite driver | `bun:sqlite` (built into Bun) |
| Dependencies | none: no `package.json`, no `node_modules` |
| **Nginx or direct** | **Direct**: serves `0.0.0.0:80` itself (it works behind Nginx on `127.0.0.1:3000` too) |

## Running it

```bash
sudo bash install.sh   # pinned Bun -> /usr/local/bin/bun
bash build.sh          # nothing to build; checks Bun is installed
SQLITE_PATH=... JWT_SECRET=... HOST=0.0.0.0 PORT=80 bash start.sh
```

## Optimizations, and why

- **Bun's native HTTP server and SQLite binding.** HTTP parsing, routing (`routes` with `:id`
  params) and keep-alive handling all happen in native code. A request runs only a little
  JavaScript: one prepared-statement call and building one string.
- **One process, one thread.** The box has one vCPU, and every query is an index or primary-key
  lookup, so the statements run synchronously on the event loop. One connection also means writes
  never wait on SQLite locks.
- **Direct instead of Nginx.** On one core, Nginx's CPU comes straight out of the app's budget.
  Idle keep-alive connections are kept for 120 s (`idleTimeout`; the spec asks for at least 65 s).
- **Rows serialize as they are.** The SQL aliases its columns to the wire keys, in wire order
  (`u.username AS author`, `... AS like_count`), so `JSON.stringify` of the rows is already the
  response, and no objects are copied per post. Fixed responses (likes, errors, health) are built
  as template strings.
- **Prepared statements**, prepared once at startup. They cache the query plan, not the data. A like
  is one `INSERT … SELECT … WHERE EXISTS (post) ON CONFLICT DO NOTHING`, and only when it inserts
  nothing does a second lookup tell "already liked" (200) from "no such post" (404).
- **Pragmas**: `journal_mode=WAL`, `synchronous=NORMAL` (rule 6), 1 GiB `mmap_size`, 32 MiB page
  cache. Writes are autocommit statements, committed before the response is built.
- **JWT verified on every request** (rule 5) with `node:crypto` HMAC-SHA256 and a constant-time
  compare. Only HS256 is accepted, and `exp` and `nbf` are checked.

## Behaviour notes

- Bodies are trimmed with `String.prototype.trim()`, and the 500 limit counts Unicode code points
  (as SQLite's `length()` does), not UTF-16 units.

## License

MIT, under the repo's [license](../../LICENSE).
