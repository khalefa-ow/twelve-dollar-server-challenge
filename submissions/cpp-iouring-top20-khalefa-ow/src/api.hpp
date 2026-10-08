#pragma once
#include "store.hpp"

namespace top20 {

struct Mutation {
  enum class Kind { Create, Like } kind;
  int64_t id = 0, user = 0;
  std::string body, author;
};

// Own all request data before returning the selected io_uring receive buffer.
// A recognized but invalid mutation gets an immediate response; only valid writes are queued.
bool prepare_mutation(const Request& r, Mutation& m, std::string& b, int& status) {
  auto path = r.path.substr(0, r.path.find('?'));
  if (r.method != "POST") return false;
  std::string_view raw;
  if (path == "/posts") m.kind = Mutation::Kind::Create;
  else {
    if (path.substr(0, 7) != "/posts/" || path.size() <= 7) return false;
    auto rest = path.substr(7);
    auto slash = rest.find('/');
    if (!slash || slash == std::string_view::npos || rest.substr(slash) != "/like") return false;
    m.kind = Mutation::Kind::Like;
    raw = rest.substr(0, slash);
  }
  AuthUser user;
  auto auth = authenticate(r.auth, r.has_auth, &user);
  if (auth != AuthResult::kOk) { status = auth_error(b, auth); return false; }
  m.user = user.id;
  if (m.kind == Mutation::Kind::Like) {
    int valid = parse_id(raw, &m.id);
    if (valid != 1) {
      status = error(b, valid ? 404 : 400, valid ? "post not found" : "invalid post id");
      return false;
    }
  } else {
    JType type;
    JField field[] = {{"body"}};
    if (!JsonParser(r.body).parse(&type, field, 1)) {
      status = error(b, 400, "malformed JSON body"); return false;
    }
    if (type != J_OBJ || field[0].type != J_STR || trim_js(field[0].str).empty()) {
      status = error(b, 400, "body is required"); return false;
    }
    auto text = trim_js(field[0].str);
    if (utf8_length(text) > 500) {
      status = error(b, 400, "body must be at most 500 characters"); return false;
    }
    m.body.assign(text);
    m.author = std::move(user.username);
  }
  return true;
}
int apply_mutation(Store& store, Mutation& m, std::string& b) {
  if (m.kind == Mutation::Kind::Create) {
    m.id = store.create(m.body, m.author);
    b.append("{\"post\":");
    b.append(store.find(m.id)->json);
    b.push_back('}');
    return 201;
  }
  int result = store.like(m.id, m.user);
  if (!result) return error(b, 404, "post not found");
  b.append(result == 1 ? "{\"liked\":true,\"already_liked\":false,\"post_id\":"
                       : "{\"liked\":true,\"already_liked\":true,\"post_id\":");
  append_int(b, m.id);
  b.push_back('}');
  return result == 1 ? 201 : 200;
}
int handle_health(std::string& b) {
  b.append("{\"status\":\"ok\",\"store\":\"memory-wal\",\"uptime_s\":");
  append_int(b, monotonic_s() - g_start_s);
  b.push_back('}');
  return 200;
}
int handle_get(std::string& b, std::string_view raw) {
  int64_t id;
  int valid = parse_id(raw, &id);
  if (!valid) return error(b, 400, "invalid post id");
  const Post* p = valid == 1 ? g_store.find(id) : nullptr;
  if (!p) return error(b, 404, "post not found");
  b.append("{\"post\":");
  b.append(p->json);
  b.push_back('}');
  return 200;
}
int dispatch_read(const Request& r, std::string& b) {
  auto path = r.path.substr(0, r.path.find('?'));
  if (r.method == "GET") {
    if (path == "/feed") { b.append(g_store.feed_json()); return 200; }
    if (path == "/health") return handle_health(b);
    if (path.substr(0, 7) == "/posts/" && path.size() > 7 && path.substr(7).find('/') == std::string_view::npos)
      return handle_get(b, path.substr(7));
  }
  return error(b, 404, "not found");
}
// Portable API tests use this immediate adapter. Production queues mutations and publishes
// the candidate Store only after its WAL fsync CQE, using the same prepare/apply functions.
int dispatch(const Request& r, std::string& b) {
  try {
    Mutation m;
    int status = 0;
    if (prepare_mutation(r, m, b, status)) return apply_mutation(g_store, m, b);
    return status ? status : dispatch_read(r, b);
  } catch (const std::exception&) {
    b.clear();
    return error(b, 500, "internal server error");
  }
}

}  // namespace top20
