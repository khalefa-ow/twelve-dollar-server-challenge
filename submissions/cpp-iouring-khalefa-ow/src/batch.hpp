// Requests own their write arguments until the entire transaction has committed.
#pragma once
#include "api.hpp"

namespace challenge {

enum class WriteKind { Post, Like };
struct Write {
  WriteKind kind = WriteKind::Post;
  int64_t user_id = 0;
  int64_t post_id = 0;
  std::string username;
  std::string text;
  std::string response;
  int status = 500;
};

// A zero return value means a validated write is ready for group commit. No SQL has run yet.
int stage_write(const Request& r, std::string_view id, bool like, Write& w, std::string& b) {
  AuthUser user;
  auto ar = authenticate(r.auth, r.has_auth, &user);
  if (ar != AuthResult::kOk) return auth_error(b, ar);
  w.user_id = user.id;
  if (like) {
    int v = parse_id(id, &w.post_id);
    if (!v) return error(b, 400, "invalid post id");
    if (v == 2) return error(b, 404, "post not found");
    w.kind = WriteKind::Like;
    return 0;
  }
  JType top;
  JField field[1] = {{"body"}};
  if (!JsonParser(r.body).parse(&top, field, 1)) return error(b, 400, "malformed JSON body");
  if (top != J_OBJ || field[0].type != J_STR) return error(b, 400, "body is required");
  auto text = trim_js(field[0].str);
  if (text.empty()) return error(b, 400, "body is required");
  if (utf8_length(text) > 500) return error(b, 400, "body must be at most 500 characters");
  w.kind = WriteKind::Post;
  w.text.assign(text);
  w.username = std::move(user.username);
  return 0;
}

int dispatch(const Request& r, std::string& b, Write& w) {
  auto path = r.path.substr(0, r.path.find('?'));
  bool get = r.method == "GET", post = r.method == "POST";
  if (get && path == "/feed") return handle_feed(b);
  if (get && path == "/health") return handle_health(b);
  if (post && path == "/posts") return stage_write(r, {}, false, w, b);
  if (path.substr(0, 7) == "/posts/" && path.size() > 7) {
    auto rest = path.substr(7);
    auto slash = rest.find('/');
    if (slash == std::string_view::npos && get) return handle_get_post(b, rest);
    if (post && slash > 0 && slash != std::string_view::npos && rest.substr(slash) == "/like")
      return stage_write(r, rest.substr(0, slash), true, w, b);
  }
  return error(b, 404, "not found");
}

class GroupCommit {
 public:
  GroupCommit() : begin_(prepare("BEGIN IMMEDIATE")), commit_(prepare("COMMIT")),
                  rollback_(prepare("ROLLBACK")) {}
  ~GroupCommit() {
    sqlite3_finalize(begin_);
    sqlite3_finalize(commit_);
    sqlite3_finalize(rollback_);
  }
  uint64_t batches = 0, requests = 0, failures = 0, largest = 0;

  // The caller must not send ANY of these responses until this method has returned.
  // On any statement/commit failure, roll back the whole group and replace all its responses.
  bool run(const std::vector<Write*>& writes) {
    if (writes.empty()) return true;
    bool ok = execute(begin_) == SQLITE_DONE;
    for (Write* w : writes) {
      if (!ok) break;
      w->response.clear();
      ok = w->kind == WriteKind::Post ? create(*w) : like(*w);
    }
    if (ok) ok = execute(commit_) == SQLITE_DONE;
    if (!ok) {
      if (!sqlite3_get_autocommit(g_db)) execute(rollback_);
      // A failed rollback leaves the connection unsafe for future requests.
      if (!sqlite3_get_autocommit(g_db)) die("cannot roll back write batch", sqlite3_errmsg(g_db));
      for (Write* w : writes) {
        w->response.clear();
        w->status = error(w->response, 500, "internal server error");
      }
      ++failures;
      return false;
    }
#ifdef CHALLENGE_CACHE_FEED
    // Feed contains exactly the latest 20 posts and their live like counts. Any committed
    // mutation can affect either membership or ordering, so rebuild lazily on the next read.
    g_feed_cache.clear();
#endif
    ++batches;
    requests += writes.size();
    if (writes.size() > largest) largest = writes.size();
    return true;
  }

 private:
  sqlite3_stmt *begin_, *commit_, *rollback_;
  static int execute(sqlite3_stmt* st) {
    int rc = sqlite3_step(st);
    sqlite3_reset(st);
    return rc;
  }
  static bool create(Write& w) {
    auto* st = g_st_insert_post;
    int rc = sqlite3_bind_int64(st, 1, w.user_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_text(st, 2, w.text.data(),
                                              static_cast<int>(w.text.size()), SQLITE_STATIC);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    if (rc == SQLITE_ROW) {
      w.post_id = sqlite3_column_int64(st, 0);
      auto& b = w.response;
      b.append("{\"post\":{\"id\":");
      append_int(b, w.post_id);
      b.append(",\"body\":");
      append_json_str(b, w.text);
      b.append(",\"created_at\":");
      append_json_str(b, reinterpret_cast<const char*>(sqlite3_column_text(st, 1)),
                      sqlite3_column_bytes(st, 1));
      b.append(",\"author\":");
      append_json_str(b, w.username);
      b.append(",\"like_count\":0}}");
      rc = sqlite3_step(st);  // RETURNING must be stepped to completion before COMMIT.
    }
    int reset_rc = sqlite3_reset(st);
    sqlite3_clear_bindings(st);  // release the borrowed request body
    w.status = 201;
    return rc == SQLITE_DONE && reset_rc == SQLITE_OK;
  }
  static bool like(Write& w) {
    auto* st = g_st_insert_like;
    int rc = sqlite3_bind_int64(st, 1, w.user_id);
    if (rc == SQLITE_OK) rc = sqlite3_bind_int64(st, 2, w.post_id);
    if (rc == SQLITE_OK) rc = sqlite3_step(st);
    int reset_rc = sqlite3_reset(st);
    if (rc != SQLITE_DONE || reset_rc != SQLITE_OK) return false;
    bool inserted = sqlite3_changes(g_db) == 1;
    if (!inserted) {
      rc = sqlite3_bind_int64(g_st_post_exists, 1, w.post_id);
      if (rc == SQLITE_OK) rc = sqlite3_step(g_st_post_exists);
      sqlite3_reset(g_st_post_exists);
      if (rc == SQLITE_DONE) {
        w.status = error(w.response, 404, "post not found");
        return true;
      }
      if (rc != SQLITE_ROW) return false;
    }
    w.response.append(inserted ? "{\"liked\":true,\"already_liked\":false,\"post_id\":"
                               : "{\"liked\":true,\"already_liked\":true,\"post_id\":");
    append_int(w.response, w.post_id);
    w.response.push_back('}');
    w.status = inserted ? 201 : 200;
    return true;
  }
};

void close_db() {
  for (auto* st : {g_st_ping, g_st_feed, g_st_post, g_st_insert_post,
                   g_st_insert_like, g_st_post_exists}) sqlite3_finalize(st);
  sqlite3_close(g_db);
  g_db = nullptr;
}

}  // namespace challenge
