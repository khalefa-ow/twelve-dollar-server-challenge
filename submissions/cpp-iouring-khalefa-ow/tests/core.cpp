// Portable checks for the production API/group-commit code. Pass a FRESH copy of seed/feed.db.
#include <fstream>
#include <iterator>
#include "../src/batch.hpp"

using namespace challenge;
namespace {
unsigned checks = 0;
void check(bool ok, const char* label) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
  ++checks;
}
int64_t scalar(sqlite3* db, const std::string& sql) {
  sqlite3_stmt* st = nullptr;
  check(sqlite3_prepare_v2(db, sql.c_str(), -1, &st, nullptr) == SQLITE_OK, "prepare observer query");
  check(sqlite3_step(st) == SQLITE_ROW, "observer query returns row");
  auto value = sqlite3_column_int64(st, 0);
  sqlite3_finalize(st);
  return value;
}
std::string read_file(const std::string& path) {
  std::ifstream f(path, std::ios::binary);
  check(f.good(), "open golden file");
  std::string s((std::istreambuf_iterator<char>(f)), {});
  while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
  return s;
}
std::string encode(std::string_view text) {
  std::string out((text.size() * 4 + 2) / 3 + 4, '\0');
  out.resize(b64url_encode(reinterpret_cast<const unsigned char*>(text.data()), text.size(), out.data()));
  return out;
}
std::string token(std::string_view payload, std::string_view header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}") {
  std::string data = encode(header) + "." + encode(payload);
  unsigned char hash[32];
  hmac_sha256(data.data(), data.size(), hash);
  return "Bearer " + data + "." + encode(std::string_view(reinterpret_cast<char*>(hash), sizeof hash));
}
struct Observation { sqlite3* db; int64_t first_id; bool saw_commit = false; };
int observe_commit(void* arg) {
  auto& o = *static_cast<Observation*>(arg);
  check(!scalar(o.db, "SELECT count(*) FROM posts WHERE id=" + std::to_string(o.first_id)),
        "independent connection cannot see uncommitted batch");
  o.saw_commit = true;
  return 0;
}
int deny_commit(void*, int action, const char* name, const char*, const char*, const char*) {
  return action == SQLITE_TRANSACTION && name && std::string_view(name) == "COMMIT" ? SQLITE_DENY : SQLITE_OK;
}
}

int main(int argc, char** argv) {
  if (argc != 3) { std::fprintf(stderr, "usage: core-test <fresh-db-copy> <repository-root>\n"); return 2; }
  g_start_s = monotonic_s();
  hmac_init("twelve-dollar-challenge");
  init_db(argv[1]);
  std::string root = argv[2], body;
  check(handle_feed(body) == 200 && body == read_file(root + "/test/golden/feed.json"), "golden feed bytes");
  for (auto id : {"1", "250000", "500000"}) {
    body.clear();
    check(handle_get_post(body, id) == 200 &&
          body == read_file(root + "/test/golden/post-" + id + ".json"), "golden post bytes");
  }
  sqlite3* observer = nullptr;
  check(sqlite3_open_v2(argv[1], &observer, SQLITE_OPEN_READONLY, nullptr) == SQLITE_OK, "open independent reader");
  auto base_id = scalar(observer, "SELECT max(id) FROM posts");
  std::string valid = token("{\"sub\":\"1\",\"username\":\"golden_ember_1\",\"exp\":" +
                            std::to_string(time(nullptr) + 3600) + "}");
  AuthUser user;
  check(authenticate(valid, true, &user) == AuthResult::kOk && user.id == 1, "valid HS256 token");
  auto bad = valid;
  bad.back() = bad.back() == 'a' ? 'b' : 'a';
  check(authenticate(bad, true, &user) == AuthResult::kInvalid, "bad signature");
  check(authenticate("Basic abc", true, &user) == AuthResult::kMissing, "bearer prefix");
  check(authenticate(token("{\"sub\":\"1\",\"username\":\"u\",\"exp\":1}"), true, &user) == AuthResult::kInvalid,
        "expired token");
  check(authenticate(token("{\"sub\":\"1\",\"username\":\"u\",\"exp\":null}"), true, &user) == AuthResult::kInvalid,
        "null expiration is invalid");
  check(authenticate(token("{\"sub\":\"1\",\"username\":\"u\",\"exp\":1e999}"), true, &user) == AuthResult::kInvalid,
        "nonfinite expiration is invalid");
  check(authenticate(token("{\"sub\":1,\"username\":\"u\"}"), true, &user) == AuthResult::kBadPayload,
        "subject must be a string");
  check(authenticate(token("{\"sub\":\"1\"}"), true, &user) == AuthResult::kBadPayload, "username required");
  check(authenticate(token("{}", "{\"alg\":\"none\"}"), true, &user) == AuthResult::kInvalid, "HS256 only");

  Request request;
  request.method = "POST";
  request.path = "/posts";
  request.auth = valid;
  request.has_auth = true;
  std::string text = "{\"body\":\"  test: <b>&amp;</b> \\\"quoted\\\" / café ✓  \"}";
  request.body = text;
  Write post;
  body.clear();
  check(dispatch(request, body, post) == 0 && post.text == "test: <b>&amp;</b> \"quoted\" / café ✓",
        "trimmed Unicode write is staged");
  check(scalar(observer, "SELECT max(id) FROM posts") == base_id, "staging does not write to SQLite");
  {
    GroupCommit group;
    Write second = post;
    second.text = "another request in the same transaction";
    Observation observation{observer, base_id + 1};
    sqlite3_commit_hook(g_db, observe_commit, &observation);
    std::vector<Write*> batch{&post, &second};
    check(group.run(batch), "two writes commit together");
    sqlite3_commit_hook(g_db, nullptr, nullptr);
    check(observation.saw_commit && group.batches == 1 && group.requests == 2 && group.largest == 2,
          "exactly one commit serves two requests");
    check(post.status == 201 && second.status == 201 && second.post_id == post.post_id + 1,
          "distinct committed post ids");
    check(scalar(observer, "SELECT count(*) FROM posts WHERE id IN (" + std::to_string(post.post_id) + "," +
                           std::to_string(second.post_id) + ")") == 2, "success is visible from independent reader");

    Write like1, like2, missing;
    for (Write* w : {&like1, &like2, &missing}) { w->kind = WriteKind::Like; w->user_id = 1; w->post_id = post.post_id; }
    missing.post_id = INT64_MAX;
    batch = {&like1, &like2, &missing};
    check(group.run(batch) && like1.status == 201 && like2.status == 200 && missing.status == 404,
          "same-batch duplicate likes and missing post");
    check(scalar(observer, "SELECT count(*) FROM likes WHERE post_id=" + std::to_string(post.post_id)) == 1,
          "exactly one committed like");
    body.clear();
    check(handle_get_post(body, std::to_string(post.post_id)) == 200 &&
          body.find("\"like_count\":1}") != std::string::npos, "live like count after commit");

    auto before_failure = scalar(observer, "SELECT max(id) FROM posts");
    Write good = post, invalid = post;
    invalid.text.clear();  // Existing schema CHECK fails after the first insert has succeeded.
    batch = {&good, &invalid};
    check(!group.run(batch) && good.status == 500 && invalid.status == 500, "statement failure fails entire group");
    check(scalar(observer, "SELECT max(id) FROM posts") == before_failure && sqlite3_get_autocommit(g_db),
          "statement failure rolls back preceding successful insert");

    sqlite3_set_authorizer(g_db, deny_commit, nullptr);
    batch = {&good};
    check(!group.run(batch) && good.status == 500, "commit failure cannot return 201");
    sqlite3_set_authorizer(g_db, nullptr, nullptr);
    check(scalar(observer, "SELECT max(id) FROM posts") == before_failure && sqlite3_get_autocommit(g_db),
          "commit failure rolls back");
    check(group.run(batch) && good.status == 201, "server recovers after rolled-back batch");
  }

  Write unused;
  for (std::string_view input : {"{}", "{\"body\":123}", "{\"body\":\"   \"}"}) {
    request.body = input;
    body.clear();
    check(dispatch(request, body, unused) == 400 && body == "{\"error\":\"body is required\"}", "body validation");
  }
  for (std::string_view input : {"{bad json", "{\"body\":\"\\ud800\"}", "{\"body\":\"\xff\"}"}) {
    request.body = input;
    body.clear();
    check(dispatch(request, body, unused) == 400 && body == "{\"error\":\"malformed JSON body\"}", "strict JSON/UTF-8");
  }
  text = "{\"body\":\"" + std::string(501, 'a') + "\"}";
  request.body = text;
  body.clear();
  check(dispatch(request, body, unused) == 400, "501 characters rejected");
  request.path = "/posts/abc/like";
  request.has_auth = false;
  body.clear();
  check(dispatch(request, body, unused) == 401, "authenticate before validating like id");

  size_t used = 0;
  Request parsed;
  std::string decoded;
  const std::string pipeline = "GET /health HTTP/1.1\r\nHost: x\r\n\r\nGET /feed HTTP/1.1\r\n\r\n";
  check(parse_request(pipeline.data(), pipeline.size(), &parsed, &used, decoded) == Parse::kOk && used == 33,
        "HTTP parser consumes only first pipelined request");
  for (std::string_view input : {
      "POST /posts HTTP/1.1\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx",
      "POST /posts HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
      "POST /posts HTTP/1.1\r\nTransfer-Encoding: gzip, chunked\r\n\r\n",
      "GET /health HTTP/9.9\r\n\r\n"})
    check(parse_request(input.data(), input.size(), &parsed, &used, decoded) == Parse::kBad, "ambiguous framing rejected");
  std::string wire = "POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  for (size_t n = 0; n < wire.size(); ++n)
    check(parse_request(wire.data(), n, &parsed, &used, decoded) == Parse::kIncomplete, "fragmented chunked request");
  check(parse_request(wire.data(), wire.size(), &parsed, &used, decoded) == Parse::kOk && parsed.body == "abc",
        "complete chunked request");
  sqlite3_close(observer);
  close_db();
  std::printf("PASS: %u core checks (golden reads, JWT/JSON, HTTP framing, group commit and rollback)\n", checks);
}
