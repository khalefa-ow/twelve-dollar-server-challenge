#pragma once
#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <memory>
#include <stdexcept>
#include <unordered_set>
#include "common.hpp"

namespace top20 {

// Fast path for all challenge users, plus arbitrary positive 64-bit JWT subjects.
class LikeSet {
 public:
  static constexpr uint64_t kDenseUsers = 65536;
  bool insert(int64_t user) {
    if (user <= 0) throw std::invalid_argument("invalid like user");
    uint64_t index = static_cast<uint64_t>(user - 1);
    if (index < kDenseUsers) {
      auto& word = dense_[index >> 6];
      uint64_t bit = uint64_t{1} << (index & 63);
      if (word & bit) return false;
      word |= bit;
    } else if (!sparse_.insert(user).second) {
      return false;
    }
    ++count_;
    return true;
  }
  int64_t size() const { return count_; }
  bool contains(int64_t user) const {
    if (user <= 0) return false;
    uint64_t index = static_cast<uint64_t>(user - 1);
    return index < kDenseUsers ? (dense_[index >> 6] & (uint64_t{1} << (index & 63))) != 0
                              : sparse_.count(user) != 0;
  }
  const auto& dense() const { return dense_; }
  const auto& sparse() const { return sparse_; }
  void restore(const std::array<uint64_t, kDenseUsers / 64>& dense,
               std::unordered_set<int64_t> sparse, int64_t count) {
    int64_t actual = static_cast<int64_t>(sparse.size());
    for (uint64_t word : dense) actual += __builtin_popcountll(word);
    for (int64_t user : sparse) if (user <= static_cast<int64_t>(kDenseUsers))
      throw std::runtime_error("invalid snapshot like user");
    if (count != actual) throw std::runtime_error("invalid snapshot like count");
    dense_ = dense;
    sparse_ = std::move(sparse);
    count_ = count;
  }
 private:
  std::array<uint64_t, kDenseUsers / 64> dense_{};
  std::unordered_set<int64_t> sparse_;
  int64_t count_ = 0;
};

bool valid_timestamp(std::string_view s) {
  if (s.size() != 24) return false;
  for (size_t i = 0; i < s.size(); ++i) {
    char delimiter = i == 4 || i == 7 ? '-' : i == 10 ? 'T' : i == 13 || i == 16 ? ':' :
                     i == 19 ? '.' : i == 23 ? 'Z' : 0;
    if (delimiter ? s[i] != delimiter : s[i] < '0' || s[i] > '9') return false;
  }
  auto pair = [&](size_t i) { return (s[i] - '0') * 10 + s[i + 1] - '0'; };
  int year = (s[0] - '0') * 1000 + (s[1] - '0') * 100 + pair(2);
  int month = pair(5), day = pair(8);
  if (month < 1 || month > 12) return false;
  constexpr int days[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  bool leap = year % 400 == 0 || (year % 4 == 0 && year % 100 != 0);
  int maximum = days[month - 1] + (month == 2 && leap);
  return day >= 1 && day <= maximum && pair(11) <= 23 && pair(14) <= 59 && pair(17) <= 59;
}

std::string utc_now() {
  timespec time;
  if (clock_gettime(CLOCK_REALTIME, &time)) throw std::runtime_error("clock_gettime failed");
  tm utc{};
  if (!gmtime_r(&time.tv_sec, &utc)) throw std::runtime_error("gmtime_r failed");
  char date[32], result[40];
  if (!std::strftime(date, sizeof date, "%Y-%m-%dT%H:%M:%S", &utc))
    throw std::runtime_error("timestamp formatting failed");
  std::snprintf(result, sizeof result, "%s.%03ldZ", date, time.tv_nsec / 1000000);
  return result;
}

struct Post {
  int64_t id = 0;
  std::string created_at, prefix, json;
  LikeSet likes;
  Post() = default;
  Post(const Post& other) : id(other.id), created_at(other.created_at), prefix(other.prefix), likes(other.likes) {
    json.reserve(prefix.size() + 24);
    refresh_json();
  }
  Post(Post&&) = default;
  Post& operator=(Post&&) = default;
  Post(int64_t post_id, std::string_view text, std::string_view author, std::string timestamp)
      : id(post_id), created_at(std::move(timestamp)) {
    prefix.append("{\"id\":");
    append_int(prefix, id);
    prefix.append(",\"body\":");
    append_json_str(prefix, text);
    prefix.append(",\"created_at\":");
    append_json_str(prefix, created_at);
    prefix.append(",\"author\":");
    append_json_str(prefix, author);
    prefix.append(",\"like_count\":");
    json.reserve(prefix.size() + 24);
    refresh_json();
  }
  void refresh_json() {
    json.assign(prefix);
    append_int(json, likes.size());
    json.push_back('}');
  }
};

class Store {
 public:
  static constexpr size_t kCapacity = 20;
  size_t size() const { return count_; }
  int64_t next_id() const { return next_id_; }
  const Post* find(int64_t id) const {
    for (const auto& p : posts_) if (p && p->id == id && id > 0) return p.get();
    return nullptr;
  }
  int64_t create(std::string_view body, std::string_view author) {
    if (!next_id_) throw std::overflow_error("post ids exhausted");
    Post p(next_id_, body, author, utc_now());
    int64_t id = p.id;
    adopt(std::make_shared<Post>(std::move(p)));
    next_id_ = id == INT64_MAX ? 0 : id + 1;
    return id;
  }
  // 0: missing, 1: first like, 2: duplicate.
  int like(int64_t id, int64_t user) {
    for (auto& p : posts_) {
      if (!p || p->id != id || id <= 0) continue;
      if (p->likes.contains(user)) return 2;
      // A staged commit shares untouched slots with the committed state. Clone only once
      // per touched post, so reads never observe an uncommitted bitmap or count.
      if (!p.unique()) p = std::make_shared<Post>(*p);
      p->likes.insert(user);
      p->refresh_json();
      feed_cache_.clear();
      return 1;
    }
    return 0;
  }
  const std::string& feed_json() {
    if (!feed_cache_.empty()) return feed_cache_;
    try {
      feed_cache_.append("{\"posts\":[");
      for (size_t i = 0; i < count_; ++i) {
        if (i) feed_cache_.push_back(',');
        feed_cache_.append(posts_[(head_ + kCapacity - 1 - i) % kCapacity]->json);
      }
      feed_cache_.append("]}");
    } catch (...) {
      feed_cache_.clear();  // Never publish a partially built cached response.
      throw;
    }
    return feed_cache_;
  }
  void load_seed(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file || file.tellg() < 0 || file.tellg() > 32 * 1024 * 1024)
      throw std::runtime_error("seed missing, unreadable, or over 32 MiB");
    file.seekg(0);
    std::string data((std::istreambuf_iterator<char>(file)), {});
    JType type;
    JField root[] = {{"next_id"}, {"posts"}};
    if (!JsonParser(data).parse(&type, root, 2) || type != J_OBJ || root[0].type != J_NUM || root[1].type != J_ARR)
      throw std::runtime_error("seed must contain integer next_id and posts array");
    int64_t next;
    if (parse_id(root[0].raw, &next) != 1) throw std::runtime_error("invalid seed next_id");
    std::vector<std::string_view> objects;
    if (!JsonParser(root[1].raw).array_items(objects, kCapacity))
      throw std::runtime_error("seed must contain at most 20 posts");
    std::vector<Post> imported;
    imported.reserve(kCapacity);
    for (auto object : objects) {
      JField f[] = {{"id"}, {"body"}, {"created_at"}, {"author"}, {"liked_user_ids"}, {"like_count"}};
      int64_t id;
      if (!JsonParser(object).parse(&type, f, 6) || type != J_OBJ || f[0].type != J_NUM ||
          parse_id(f[0].raw, &id) != 1 || f[1].type != J_STR || f[1].str.empty() ||
          utf8_length(f[1].str) > 500 || f[2].type != J_STR || !valid_timestamp(f[2].str) ||
          f[3].type != J_STR || f[4].type != J_ARR || id >= next)
        throw std::runtime_error("invalid seed post");
      for (const Post& p : imported) if (p.id == id) throw std::runtime_error("duplicate seed post id");
      Post p(id, f[1].str, f[3].str, std::move(f[2].str));
      std::vector<std::string_view> users;
      if (!JsonParser(f[4].raw).array_items(users, 1000000)) throw std::runtime_error("invalid seed likes array");
      for (auto raw : users) {
        int64_t user;
        if (parse_id(raw, &user) != 1) throw std::runtime_error("invalid seed like user");
        p.likes.insert(user);
      }
      if (f[5].type != J_NONE) {
        int64_t declared = -1;
        auto parsed = std::from_chars(f[5].raw.data(), f[5].raw.data() + f[5].raw.size(), declared);
        if (f[5].type != J_NUM || parsed.ec != std::errc{} ||
            parsed.ptr != f[5].raw.data() + f[5].raw.size() || declared != p.likes.size())
          throw std::runtime_error("seed like_count does not match membership");
      }
      p.refresh_json();
      imported.push_back(std::move(p));
    }
    // Accept either input order; initialization follows the original timestamp/id feed order.
    std::sort(imported.begin(), imported.end(), [](const Post& a, const Post& b) {
      return a.created_at != b.created_at ? a.created_at < b.created_at : a.id < b.id;
    });
    Store replacement;
    replacement.next_id_ = next;
    for (Post& p : imported) replacement.adopt(std::make_shared<Post>(std::move(p)));
    *this = std::move(replacement);  // Invalid seeds never partially replace live state.
  }
  const Post& oldest(size_t index) const {
    if (index >= count_) throw std::out_of_range("post index");
    return *posts_[(head_ + kCapacity - count_ + index) % kCapacity];
  }
  void restore_next_id(int64_t next) {
    if (next < 0) throw std::runtime_error("invalid snapshot next id");
    next_id_ = next;
  }
  void restore_post(Post&& p) {
    if (p.id <= 0 || find(p.id) || !valid_timestamp(p.created_at))
      throw std::runtime_error("invalid restored post");
    p.json.reserve(p.prefix.size() + 24);
    p.refresh_json();
    adopt(std::make_shared<Post>(std::move(p)));
  }
  void replay_post(Post&& p) {
    if (p.id != next_id_ || !next_id_) throw std::runtime_error("WAL post id out of sequence");
    int64_t next = p.id == INT64_MAX ? 0 : p.id + 1;
    restore_post(std::move(p));
    next_id_ = next;
  }
 private:
  std::array<std::shared_ptr<Post>, kCapacity> posts_{};
  size_t head_ = 0, count_ = 0;
  int64_t next_id_ = 1;
  std::string feed_cache_;
  void adopt(std::shared_ptr<Post> p) {
    posts_[head_] = std::move(p);
    head_ = (head_ + 1) % kCapacity;
    if (count_ < kCapacity) ++count_;
    feed_cache_.clear();
  }
};
Store g_store;

}  // namespace top20
