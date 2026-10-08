The portable `core.cpp` checks execute the **production** parser, JWT, SQL and group-commit code.
They check golden response bytes, independent-reader visibility before/after commit, duplicate
likes in one group, schema-constraint rollback, injected COMMIT failure, recovery, UTF-8, and HTTP
framing. They do not simulate or validate the Linux io_uring transport.

On Linux, after `bash build.sh`, from the repository root:

```bash
scratch=$(mktemp -d)
cp seed/feed.db "$scratch/feed.db"
submissions/cpp-iouring-khalefa-ow/bin/core-test "$scratch/feed.db" "$PWD"
bash test/run.sh submissions/cpp-iouring-khalefa-ow
python3 submissions/cpp-iouring-khalefa-ow/tests/transport.py
```

`transport.py` starts its own server on a fresh database copy, uses raw sockets to exercise
fragmented and pipelined requests, concurrent writes, client disconnects, and validates committed
rows through another SQLite connection. Pass `--connections 15000` to also hold that many idle
keep-alive connections. It needs Python 3.10+ and a sufficiently high descriptor limit.

To compile the portable core checks on macOS, use C++17, SQLite 3.35+ and OpenSSL. Supply your
OpenSSL include/library paths and compile SQLite as C before linking the test with `-lcrypto`.
The `server.cpp` file and `build.sh` intentionally require Linux.
