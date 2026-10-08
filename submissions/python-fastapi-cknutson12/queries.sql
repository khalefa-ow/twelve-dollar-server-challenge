-- SQLite schema, seed-population notes, and SQL extracted from app.py.
-- Apply the schema block to an empty database.
-- Execute application statements independently.
-- Bind the documented parameters before executing parameterized statements.

-- Schema (../../schema.sql; applied by seed/make-seed.sh).
-- Timestamps are UTC TEXT: YYYY-MM-DDTHH:MM:SS.mmmZ.
CREATE TABLE users (
  id          INTEGER PRIMARY KEY,
  username    TEXT NOT NULL UNIQUE,
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE posts (
  id          INTEGER PRIMARY KEY,
  user_id     INTEGER NOT NULL REFERENCES users(id),
  body        TEXT NOT NULL CHECK (length(body) BETWEEN 1 AND 500),
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now'))
);

CREATE TABLE likes (
  user_id     INTEGER NOT NULL REFERENCES users(id),
  post_id     INTEGER NOT NULL REFERENCES posts(id),
  created_at  TEXT NOT NULL DEFAULT (strftime('%Y-%m-%dT%H:%M:%fZ', 'now')),
  PRIMARY KEY (user_id, post_id)
);

CREATE INDEX posts_created_at_id_idx ON posts (created_at DESC, id DESC);
CREATE INDEX posts_user_id_idx       ON posts (user_id);
CREATE INDEX likes_post_id_idx       ON likes (post_id);

-- Database population (seed/generate.mjs and seed/make-seed.sh).
-- Run from the repository root with Node.js >= 18 and the sqlite3 CLI:
--   bash seed/make-seed.sh
--
-- The generator uses mulberry32 with seed 42 to produce deterministic TSV rows:
--   users.tsv: 50,000 users with unique adjective_noun_id usernames.
--   posts.tsv: 500,000 posts spread over 2025, each referencing a seeded user.
--   likes.tsv: approximately 2 million likes, with distinct users per post.
-- It also creates seed/tokens.json: HS256 tokens for the first 20,000 users.
-- Tokens use the current issuance time and are valid for 10 years.
--
-- The seed script recreates seed/feed.db, applies schema.sql, and bulk-imports
-- the TSV files. The following are sqlite3 CLI commands, shown as comments:
--   .mode ascii
--   .separator "\t" "\n"
--   .import seed/tsv/users.tsv users
--   .import seed/tsv/posts.tsv posts
--   .import seed/tsv/likes.tsv likes
-- It runs ANALYZE and VACUUM, switches to WAL, checkpoints the database,
-- removes the temporary TSV files, and verifies an expected content SHA-256.
-- Test runs use a fresh copy of seed/feed.db at the SQLITE_PATH environment path.
-- During normal operation, POST /posts and POST /posts/{post_id}/like add rows.

-- Application queries (app.py).

-- Database setup (app.py:31).
PRAGMA journal_mode = WAL;
PRAGMA synchronous = NORMAL;
PRAGMA busy_timeout = 5000;
PRAGMA mmap_size = 1073741824;
PRAGMA cache_size = -65536;
PRAGMA temp_store = MEMORY;

-- GET /health (app.py:146).
SELECT 1;

-- FEED_SQL: GET /feed (app.py:43).
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id
 ORDER BY p.created_at DESC, p.id DESC LIMIT 20;

-- POST_SQL: GET /posts/{post_id} (app.py:44).
-- Parameters: 1 = post_id.
SELECT p.id, p.body, p.created_at, u.username,
       (SELECT count(*) FROM likes l WHERE l.post_id = p.id)
  FROM posts p JOIN users u ON u.id = p.user_id
 WHERE p.id = ?;

-- INSERT_POST_SQL: POST /posts (app.py:45).
-- Parameters: 1 = authenticated user_id, 2 = trimmed body.
INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at;

-- INSERT_LIKE_SQL: POST /posts/{post_id}/like (app.py:47).
-- Parameters: ?1 = authenticated user_id, ?2 = post_id.
INSERT INTO likes (user_id, post_id)
SELECT ?1, ?2 WHERE EXISTS (SELECT 1 FROM posts WHERE id = ?2)
ON CONFLICT (user_id, post_id) DO NOTHING;

-- POST_EXISTS_SQL: distinguish an existing like from a missing post (app.py:52).
-- Parameters: 1 = post_id.
SELECT 1 FROM posts WHERE id = ?;
