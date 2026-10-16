/* db.h - SQLite layer: statements, feed/post/like/create/health */
#pragma once
#include <stddef.h>
#include <stdint.h>

void db_open(const char *path);
void db_close(void);

/* Group transaction: opened lazily by the first write of a loop batch,
 * committed by db_end_batch() before any response of the batch is sent. */
int db_txn_begin(void);
int db_commit(void);   /* 0 failed (already rolled back), 1 ok */
int db_in_txn(void);
void db_end_batch(void);

void h_health(int slot);
void h_feed(int slot);
void h_post(int slot, int64_t id);
void h_like(int slot, int64_t uid, int64_t pid);
void h_create(int slot, int64_t uid, const char *uname, size_t ulen,
              const char *body, size_t blen);

/* shared response scratch (thread-local in db.c, single-threaded server) */
extern char db_scratch_ok;
