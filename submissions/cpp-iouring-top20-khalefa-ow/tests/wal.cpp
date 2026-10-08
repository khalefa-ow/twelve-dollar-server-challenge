// Real on-disk journal and production batch preparation, portable to Linux/macOS.
#include <sys/wait.h>
#include "../src/batch.hpp"
using namespace top20;
namespace {
unsigned checks = 0;
void check(bool ok, const char* label) {
  if (!ok) { std::fprintf(stderr, "FAIL: %s\n", label); std::exit(1); }
  ++checks;
}
template<class F> void rejects(F&& f, const char* label) {
  try { f(); } catch (const std::exception&) { check(true, label); return; }
  check(false, label);
}
struct Temp {
  std::string path;
  Temp() {
    char name[] = "/tmp/top20-wal-XXXXXX";
    char* result = mkdtemp(name);
    if (!result) disk::fail("mkdtemp");
    path = result;
  }
  ~Temp() { std::filesystem::remove_all(path); }
};
Mutation create(std::string body = "durable café ✓") {
  Mutation m;
  m.kind = Mutation::Kind::Create;
  m.body = std::move(body);
  m.author = "wal-test";
  return m;
}
Mutation like(int64_t id, int64_t user) {
  Mutation m;
  m.kind = Mutation::Kind::Like;
  m.id = id; m.user = user;
  return m;
}
void commit(Store& s, Journal& j, std::vector<Mutation> writes) {
  WriteBatch batch;
  batch.prepare(s, j, writes);
  if (!batch.frame.empty()) {
    // These real IO calls model the completed write/fsync CQEs in portable tests.
    disk::write_at(j.fd(), batch.frame, j.offset());
    disk::sync(j.fd());
  }
  batch.publish(s, j);
}
std::string file(const std::string& name) {
  std::ifstream input(name, std::ios::binary);
  return std::string(std::istreambuf_iterator<char>(input), {});
}
void file(const std::string& name, std::string_view bytes) {
  disk::Fd fd{::open(name.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644)};
  if (fd.value < 0) disk::fail("open test file");
  disk::write_at(fd.value, bytes, 0);
  disk::sync(fd.value);
}
void copy_store(const std::string& from, const std::string& to) {
  file(to + "/snapshot.bin", file(from + "/snapshot.bin"));
  file(to + "/journal.wal", file(from + "/journal.wal"));
}
}
int main() {
  check(disk::crc32c("123456789") == 0xe3069283u, "standard CRC32C vector");
  Temp directory;
  std::string expected;
  uint64_t sequence;
  {
    Journal journal;
    Store committed;
    journal.open(directory.path, nullptr, committed);
    check(committed.size() == 0 && committed.next_id() == 1, "fresh persistent store");
    rejects([&] { Journal second; Store s; second.open(directory.path, nullptr, s); }, "exclusive writer lock");
    commit(committed, journal, {create()});
    const auto* untouched = committed.find(1);
    auto old_feed = committed.feed_json();
    WriteBatch batch;
    std::vector<Mutation> writes{like(1, 1), like(1, 1), like(1, 65536), like(1, 65537), like(1, INT64_MAX)};
    batch.prepare(committed, journal, writes);
    check(committed.find(1) == untouched && committed.find(1)->likes.size() == 0 && committed.feed_json() == old_feed,
          "pending commit leaves committed reads unchanged");
    check(batch.candidate.find(1) != untouched && batch.candidate.find(1)->likes.size() == 4, "copy-on-write staged count");
    check(batch.results[0].status == 201 && batch.results[1].status == 200, "in-group duplicate detection");
    check(batch.results.size() == 5, "one result for each grouped query");
    auto offset = journal.offset();
    disk::write_at(journal.fd(), batch.frame, offset);
    disk::sync(journal.fd());
    check(committed.find(1)->likes.size() == 0, "sync alone does not publish candidate");
    batch.publish(committed, journal);
    check(committed.find(1)->likes.size() == 4 && journal.sequence() == 2, "durable group publishes count");
    check(journal.offset() == offset + static_cast<off_t>(batch.frame.size()), "group uses one WAL frame");
    batch = WriteBatch();
    auto wal_size = journal.offset();
    auto seq = journal.sequence();
    commit(committed, journal, {like(1, 65537), like(99, 1)});
    check(journal.offset() == wal_size && journal.sequence() == seq, "duplicate/missing likes skip WAL write and sync");
    std::vector<Mutation> wrap;
    for (int i = 0; i < 45; ++i) wrap.push_back(create("wrap " + std::to_string(i)));
    wrap.push_back(like(46, 1)); wrap.push_back(like(46, 65536));
    wrap.push_back(like(46, 65537)); wrap.push_back(like(46, INT64_MAX));
    commit(committed, journal, std::move(wrap));
    check(committed.size() == 20 && !committed.find(1) && !committed.find(26) && committed.find(27), "grouped eviction");
    expected = committed.feed_json();
    sequence = journal.sequence();
  }
  {
    Journal journal; Store s;
    journal.open(directory.path, "/missing/seed-is-ignored-on-restart", s);
    check(s.feed_json() == expected && s.next_id() == 47 && journal.sequence() == sequence, "restart replays exact feed and next id");
    for (int64_t uid : {int64_t{1}, int64_t{65536}, int64_t{65537}, INT64_MAX})
      check(s.like(46, uid) == 2, "replay restores per-user membership");
    check(s.find(46)->likes.size() == 4, "replay restores precomputed count");
    journal.checkpoint(s);
    check(journal.offset() == 0 && file(directory.path + "/journal.wal").empty(), "snapshot truncates covered WAL");
  }
  {
    Journal journal; Store s;
    journal.open(directory.path, nullptr, s);
    check(s.feed_json() == expected && s.find(46)->likes.size() == 4, "snapshot restores cached JSON and stored count");
    commit(s, journal, {like(46, 7), create("post after checkpoint"), like(47, 65537)});
    expected = s.feed_json();
    check(s.next_id() == 48 && journal.sequence() == sequence + 1, "post-checkpoint group continues sequence");
  }
  // Every prefix of a frame is discarded as a whole; no partial post/like is published.
  std::string log = file(directory.path + "/journal.wal");
  check(log.size() > disk::kHeader, "test has nonempty frame");
  for (size_t cut = 0; cut < log.size(); ++cut) {
    Temp torn;
    file(torn.path + "/snapshot.bin", file(directory.path + "/snapshot.bin"));
    file(torn.path + "/journal.wal", std::string_view(log).substr(0, cut));
    Journal journal; Store s;
    journal.open(torn.path, nullptr, s);
    check(s.next_id() == 47 && s.find(46)->likes.size() == 4 && !s.find(47), "torn group is atomic");
    check(journal.offset() == 0 && file(torn.path + "/journal.wal").empty(), "torn frame is truncated before new appends");
    commit(s, journal, {create("after torn frame")});
    check(s.find(47) && s.next_id() == 48, "write after truncated tail");
  }
  {
    Journal j; Store s;
    j.open(directory.path, nullptr, s);
    check(s.feed_json() == expected && s.find(46)->likes.size() == 5 && s.find(47)->likes.size() == 1, "full frame restores all operations");
  }
  // Corrupt complete data is an error, not silently mistaken for an incomplete tail.
  for (size_t byte : {size_t{8}, size_t{16}, size_t{24}, disk::kHeader + 2, log.size() - 1}) {
    Temp corrupt;
    copy_store(directory.path, corrupt.path);
    auto bytes = log; bytes[byte] ^= 1;
    file(corrupt.path + "/journal.wal", bytes);
    rejects([&] { Journal j; Store s; j.open(corrupt.path, nullptr, s); }, "complete frame corruption fails closed");
  }
  {
    Temp corrupt;
    copy_store(directory.path, corrupt.path);
    auto bytes = file(corrupt.path + "/snapshot.bin"); bytes[disk::kHeader + 15] ^= 1;
    file(corrupt.path + "/snapshot.bin", bytes);
    rejects([&] { Journal j; Store s; j.open(corrupt.path, nullptr, s); }, "snapshot corruption fails closed");
  }
  {
    Temp gap;
    file(gap.path + "/snapshot.bin", file(directory.path + "/snapshot.bin"));
    auto h = disk::header(std::string_view(log).substr(0, disk::kHeader), disk::kWalMagic);
    file(gap.path + "/journal.wal", disk::frame(disk::kWalMagic, h.sequence + 1,
         std::string_view(log).substr(disk::kHeader, h.length)));
    rejects([&] { Journal j; Store s; j.open(gap.path, nullptr, s); }, "checksummed sequence gap fails closed");
  }
  {
    Store original;
    original.create("unchanged", "user");
    auto before = original.feed_json();
    std::string operations;
    disk::append_like(operations, 1, 7, 1);
    operations.push_back(99);  // Error after an otherwise valid mutation.
    rejects([&] { disk::replay(original, operations); }, "invalid operation rejects entire replay group");
    check(original.feed_json() == before && original.find(1)->likes.size() == 0, "failed replay preserves committed bitmap/count");
    operations.clear();
    disk::append_like(operations, 1, 7, 2);
    rejects([&] { disk::replay(original, operations); }, "WAL resulting counter must match membership");
  }
  // Simulate a crash between durable snapshot rename and WAL truncation. Covered frames
  // must be skipped, rather than duplicating likes or failing the post id sequence.
  {
    Journal j; Store s;
    j.open(directory.path, nullptr, s);
    std::string covered = file(directory.path + "/journal.wal");
    j.checkpoint(s);
    file(directory.path + "/journal.wal", covered);
  }
  {
    Journal j; Store s;
    j.open(directory.path, nullptr, s);
    check(s.feed_json() == expected && s.find(46)->likes.size() == 5, "checkpoint rename/truncate crash window");
    check(j.offset() == 0, "recovery retires log fully covered by snapshot");
    commit(s, j, {like(47, 1)});
    expected = s.feed_json();
  }
  {
    Journal j; Store s;
    j.open(directory.path, nullptr, s);
    check(s.feed_json() == expected && s.find(47)->likes.size() == 2, "new frame after covered frames");
  }
  {
    Temp rollback;
    Journal j; Store s;
    j.open(rollback.path, nullptr, s);
    commit(s, j, {create()});
    WriteBatch b; std::vector<Mutation> m{like(1, 1)};
    b.prepare(s, j, m);
    disk::write_at(j.fd(), std::string_view(b.frame).substr(0, b.frame.size() / 2), j.offset());
    j.rollback_uncommitted();
    check(s.find(1)->likes.size() == 0 && static_cast<off_t>(file(rollback.path + "/journal.wal").size()) == j.offset(),
          "failed append rolls back without publishing");
    commit(s, j, {like(1, 1)});
    check(s.find(1)->likes.size() == 1, "retry after failed append");
  }
  // Actual process exit without destructors/checkpoint, followed by reopening the disk state.
  {
    Temp crash;
    pid_t child = fork();
    if (child < 0) disk::fail("fork");
    if (!child) {
      Journal j; Store s;
      j.open(crash.path, nullptr, s);
      commit(s, j, {create(), like(1, 65537)});
      _exit(0);
    }
    int status;
    check(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "writer exits without cleanup");
    Journal j; Store s;
    j.open(crash.path, nullptr, s);
    check(s.next_id() == 2 && s.find(1)->likes.size() == 1 && s.like(1, 65537) == 2, "crash recovery preserves acknowledged writes");
  }
  {
    Temp initial;
    auto nested = initial.path + "/nested/store";
    auto seed = initial.path + "/seed.json";
    file(seed, "{\"next_id\":9223372036854775807,\"posts\":[{\"id\":9007199254741001,\"body\":\"seed\","
               "\"created_at\":\"2025-12-31T23:59:59.000Z\",\"author\":\"user\",\"liked_user_ids\":[1,65537],\"like_count\":2}]}");
    std::string feed;
    {
      Journal j; Store s;
      j.open(nested, seed.c_str(), s);
      check(s.next_id() == INT64_MAX && s.find(9007199254741001)->likes.size() == 2, "seed initializes persisted counts and exact ids");
      commit(s, j, {create("last id"), like(INT64_MAX, INT64_MAX)});
      check(s.next_id() == 0 && s.find(INT64_MAX), "final signed 64-bit post id");
      rejects([&] { commit(s, j, {create("overflow")}); }, "exhausted ids reject new posts");
      j.checkpoint(s);
      feed = s.feed_json();
    }
    std::filesystem::remove(seed);
    Journal j; Store s;
    j.open(nested, seed.c_str(), s);
    check(s.next_id() == 0 && s.feed_json() == feed && s.like(INT64_MAX, INT64_MAX) == 2,
          "snapshot recovers exhausted counter without seed file");
  }
  {
    Temp unacknowledged;
    {
      Journal j; Store s;
      j.open(unacknowledged.path, nullptr, s);
      WriteBatch batch;
      std::vector<Mutation> writes{create(), like(1, 1)};
      batch.prepare(s, j, writes);
      disk::write_at(j.fd(), batch.frame, j.offset());
      // No publish and no reply: a complete frame may still survive a process crash.
      check(!s.find(1), "written but unacknowledged frame stays hidden from live reads");
    }
    Journal j; Store s;
    j.open(unacknowledged.path, nullptr, s);
    check(s.find(1) && s.find(1)->likes.size() == 1 && s.like(1, 1) == 2,
          "complete unacknowledged frame recovers atomically");
  }
  // Validate stored counters once at startup; read paths never scan memberships.
  {
    Store s; s.create("counter", "user"); s.like(1, 1);
    auto bytes = disk::snapshot(s);
    disk::Reader r{bytes}; r.id(true); r.u32(); disk::post(r);
    size_t count_offset = bytes.size() - r.in.size();
    bytes[count_offset] = 2;
    rejects([&] { disk::snapshot(bytes); }, "snapshot count/membership mismatch");
  }
  std::printf("PASS: %u WAL/batch checks (sync, recovery, torn tails, checkpoints, membership, counts, crash exit)\n", checks);
}
