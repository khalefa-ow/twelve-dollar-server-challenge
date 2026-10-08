// Executes the production in-memory store and API without any Linux or database dependency.
#include <fstream>
#include <iterator>
#include <memory>
#include "../src/api.hpp"

using namespace top20;
namespace {
unsigned checks = 0;
void check(bool ok, const char* label) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
  ++checks;
}
std::string read_file(const char* path) {
  std::ifstream f(path, std::ios::binary);
  check(f.good(), "open expected output");
  std::string result((std::istreambuf_iterator<char>(f)), {});
  while (!result.empty() && (result.back() == '\n' || result.back() == '\r')) result.pop_back();
  return result;
}
std::string encode(std::string_view bytes) {
  std::string result((bytes.size() * 4 + 2) / 3 + 4, '\0');
  result.resize(b64url_encode(reinterpret_cast<const unsigned char*>(bytes.data()), bytes.size(), result.data()));
  return result;
}
std::string token(std::string_view payload, std::string_view header = "{\"alg\":\"HS256\"}") {
  std::string data = encode(header) + "." + encode(payload);
  unsigned char mac[32];
  hmac_sha256(data.data(), data.size(), mac);
  return "Bearer " + data + "." + encode(std::string_view(reinterpret_cast<char*>(mac), sizeof mac));
}
std::string signed_user(int64_t id) {
  return token("{\"sub\":\"" + std::to_string(id) + "\",\"username\":\"test-user\",\"exp\":" +
               std::to_string(time(nullptr) + 3600) + "}");
}
std::string request(std::string_view method, std::string_view path, std::string_view auth,
                    std::string_view payload, int status) {
  Request r;
  r.method = method;
  r.path = path;
  r.auth = auth;
  r.has_auth = !auth.empty();
  r.body = payload;
  std::string body;
  check(dispatch(r, body) == status, "API response status");
  JType type;
  check(JsonParser(body).parse(&type, nullptr, 0) && type == J_OBJ, "API returns valid JSON object");
  return body;
}
void check_error(std::string_view method, std::string_view path, std::string_view auth,
                 std::string_view payload, int status, std::string_view message) {
  check(request(method, path, auth, payload, status) == "{\"error\":\"" + std::string(message) + "\"}",
        "exact error bytes");
}
}

int main(int argc, char** argv) {
  if (argc == 3 && (std::string_view(argv[1]) == "--dump-seed" || std::string_view(argv[1]) == "--inspect-seed")) {
    try {
      g_store.load_seed(argv[2]);
      if (std::string_view(argv[1]) == "--inspect-seed") {
        std::string output = "{\"next_id\":";
        append_int(output, g_store.next_id());
        output.append(",\"feed\":");
        output.append(g_store.feed_json());
        output.push_back('}');
        std::puts(output.c_str());
      } else {
        std::puts(g_store.feed_json().c_str());
      }
      return 0;
    } catch (const std::exception& e) {
      std::fprintf(stderr, "invalid seed: %s\n", e.what());
      return 1;
    }
  }
  if (argc != 1 && argc != 3) { std::fprintf(stderr, "usage: core-test [seed.json golden-feed.json]\n"); return 2; }
  g_start_s = monotonic_s();
  hmac_init("twelve-dollar-challenge");
  check(g_store.size() == 0 && g_store.next_id() == 1 && g_store.feed_json() == "{\"posts\":[]}", "empty store");
  auto auth = signed_user(1);
  auto body = request("POST", "/posts", auth, "{\"body\":\"  café ✓ \\\"quote\\\" \\n  \"}", 201);
  check(body.find("\"body\":\"café ✓ \\\"quote\\\"\"") != std::string::npos, "Unicode body trimmed and escaped");
  check(g_store.find(1) && valid_timestamp(g_store.find(1)->created_at), "UTC millisecond timestamp");
  check(g_store.find(1)->created_at.substr(0, 10) == utc_now().substr(0, 10), "timestamp is current UTC date");
  check(valid_timestamp("2024-02-29T00:00:00.000Z") && !valid_timestamp("2025-02-29T00:00:00.000Z") &&
        !valid_timestamp("2025-04-31T00:00:00.000Z"), "calendar dates and leap years");
  std::string initial_feed = g_store.feed_json();
  check(request("POST", "/posts/1/like", auth, {}, 201) ==
        "{\"liked\":true,\"already_liked\":false,\"post_id\":1}", "first like");
  check(request("POST", "/posts/1/like", auth, {}, 200) ==
        "{\"liked\":true,\"already_liked\":true,\"post_id\":1}", "duplicate like");
  check(g_store.feed_json() != initial_feed && g_store.find(1)->likes.size() == 1, "like invalidates feed cache");
  for (auto user : {int64_t{64}, int64_t{65}, int64_t{65536}, int64_t{65537}, INT64_MAX}) {
    auto a = signed_user(user);
    request("POST", "/posts/1/like", a, {}, 201);
    request("POST", "/posts/1/like", a, {}, 200);
  }
  check(g_store.find(1)->likes.size() == 6 && g_store.find(1)->json.find("\"like_count\":6}") != std::string::npos,
        "bitmap boundaries and sparse 64-bit users");
  for (int64_t id = 2; id <= 20; ++id) check(g_store.create("post", "author") == id, "monotonic ids");
  check(g_store.size() == 20 && g_store.find(1), "capacity includes oldest post until next insert");
  check(g_store.create("evicts oldest", "author") == 21 && g_store.size() == 20 && !g_store.find(1), "21st post evicts oldest");
  check(g_store.find(21)->likes.size() == 0 && g_store.like(21, 1) == 1 && g_store.like(21, 65537) == 1,
        "reused ring slot inherits no dense or sparse likes");
  check_error("GET", "/posts/1", {}, {}, 404, "post not found");
  check_error("POST", "/posts/1/like", auth, {}, 404, "post not found");
  for (int64_t id = 22; id <= 1000; ++id) {
    check(g_store.create("turnover", "author") == id && g_store.size() == 20, "bounded store across many turnovers");
    check(!g_store.find(id - 20) && g_store.find(id) && g_store.find(id - 19), "exact retained window");
  }
  std::string feed = g_store.feed_json();
  JType type;
  JField posts[] = {{"posts"}};
  check(JsonParser(feed).parse(&type, posts, 1), "feed JSON valid");
  std::vector<std::string_view> entries;
  check(JsonParser(posts[0].raw).array_items(entries, 20) && entries.size() == 20, "feed has exactly 20 posts");
  for (size_t i = 0; i < entries.size(); ++i) {
    JField id[] = {{"id"}};
    int64_t value;
    check(JsonParser(entries[i]).parse(&type, id, 1) && parse_id(id[0].raw, &value) == 1 &&
          value == 1000 - static_cast<int64_t>(i), "newest-first feed order");
  }
  check_error("GET", "/posts/0", {}, {}, 400, "invalid post id");
  check_error("GET", "/posts/abc", {}, {}, 400, "invalid post id");
  check_error("GET", "/posts/99999999999999999999999", {}, {}, 404, "post not found");
  check_error("GET", "/nope", {}, {}, 404, "not found");
  check_error("POST", "/posts/abc/like", {}, {}, 401, "missing bearer token");
  check_error("POST", "/posts/abc/like", auth, {}, 400, "invalid post id");
  check_error("POST", "/posts", "Bearer garbage", "{}", 401, "invalid or expired token");
  check_error("POST", "/posts", token("{\"sub\":\"1\",\"username\":\"u\",\"exp\":1}"), "{}", 401,
              "invalid or expired token");
  check_error("POST", "/posts", token("{\"sub\":1,\"username\":\"u\"}"), "{}", 401, "invalid token payload");
  check_error("POST", "/posts", token("{}", "{\"alg\":\"none\"}"), "{}", 401, "invalid or expired token");
  for (auto text : {"{}", "{\"body\":1}", "{\"body\":\"   \"}"})
    check_error("POST", "/posts", auth, text, 400, "body is required");
  for (auto text : {"{bad json", "{\"body\":\"\\ud800\"}", "{\"body\":\"\xff\"}"})
    check_error("POST", "/posts", auth, text, 400, "malformed JSON body");
  check_error("POST", "/posts", auth, "{\"body\":\"" + std::string(501, 'a') + "\"}", 400,
              "body must be at most 500 characters");
  request("POST", "/posts", auth, "{\"body\":\"" + std::string(500, 'a') + "\"}", 201);
  check(request("GET", "/health", {}, {}, 200).find("\"store\":\"memory-wal\"") != std::string::npos, "health names memory WAL store");

  std::string chunked = "POST /posts HTTP/1.1\r\nTransfer-Encoding: chunked\r\n\r\n3\r\nabc\r\n0\r\n\r\n";
  Request parsed;
  size_t used;
  std::string decoded;
  for (size_t i = 0; i < chunked.size(); ++i)
    check(parse_request(chunked.data(), i, &parsed, &used, decoded) == Parse::kIncomplete, "chunked fragmentation");
  check(parse_request(chunked.data(), chunked.size(), &parsed, &used, decoded) == Parse::kOk && parsed.body == "abc",
        "chunked body parsed");
  const std::string ambiguous = "POST /posts HTTP/1.1\r\nContent-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\nx";
  check(parse_request(ambiguous.data(), ambiguous.size(), &parsed, &used, decoded) == Parse::kBad, "ambiguous framing rejected");
  check(!JsonParser("[1,]").array_items(entries, 20) && !JsonParser("[1,2]").array_items(entries, 1),
        "seed arrays reject trailing commas and bounds overflow");

  if (argc == 3) {
    g_store.load_seed(argv[1]);
    check(g_store.size() == 20 && g_store.next_id() == 500001, "seed retains only top20 and global next id");
    check(g_store.feed_json() == read_file(argv[2]), "TSV-exported seed matches golden feed bytes");
    check(!g_store.find(1) && !g_store.find(250000) && g_store.find(500000), "seed excludes historical posts");
    check(g_store.create("seeded insert", "author") == 500001 && !g_store.find(499981), "seed eviction and id continuity");
  }
  std::printf("PASS: %u top20 core checks (bounded retention, per-user likes, cache invalidation, JWT/JSON/HTTP)\n", checks);
}
