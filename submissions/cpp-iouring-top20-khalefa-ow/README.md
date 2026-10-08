# C++17 + io_uring + cached latest-20 feed

This is the spec-compatible optimized io_uring submission. It retains the complete post and like
history in the supplied SQLite database, while caching the serialized `/feed` response because the
feed is always exactly the latest 20 posts. A successful create or like commit invalidates the
cache; the next feed request rebuilds it from authoritative data, including current like counts.

The networking, parsing, authentication, group-commit, and persistence behavior is shared with
`cpp-iouring-khalefa-ow`. Historical `GET /posts/:id` and `POST /posts/:id/like` requests therefore
remain valid after newer posts are created. `/health` also retains the exact required database
health response.

From the repository root on Linux:

```bash
sudo bash submissions/cpp-iouring-top20-khalefa-ow/install.sh
bash seed/make-seed.sh
bash test/run.sh submissions/cpp-iouring-top20-khalefa-ow

scratch=$(mktemp -d)
cp seed/feed.db "$scratch/feed.db"
submissions/cpp-iouring-top20-khalefa-ow/bin/core-test "$scratch/feed.db" "$PWD"
python3 submissions/cpp-iouring-top20-khalefa-ow/tests/transport.py
```

For a standalone run, set `SQLITE_PATH`, `JWT_SECRET`, `HOST`, and `PORT` as described in
[`SPEC.md`](../../SPEC.md). `HOST` defaults to `0.0.0.0` and `PORT` defaults to `80`.

The feed cache is process-local and deliberately small: one compact JSON response for 20 posts.
It is never used for historical post lookup and never survives a committed mutation.

MIT, under the repository's [license](../../LICENSE).
