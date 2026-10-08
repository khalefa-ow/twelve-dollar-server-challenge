#pragma once
#include <filesystem>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#include "store.hpp"

namespace top20 {
namespace disk {
constexpr size_t kHeader = 28, kFooter = 8;
constexpr size_t kMaxFrame = 16 * 1024 * 1024;
constexpr std::string_view kWalMagic = "T20WAL01", kSnapshotMagic = "T20SNP01", kDone = "T20DONE!";

// Castagnoli CRC. The portable table also runs in native recovery tests on macOS.
uint32_t crc32c(std::string_view bytes) {
  static const auto table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < t.size(); ++i) {
      uint32_t c = i;
      for (int j = 0; j < 8; ++j) c = (c >> 1) ^ (0x82f63b78u & (0u - (c & 1)));
      t[i] = c;
    }
    return t;
  }();
  uint32_t c = ~0u;
  for (unsigned char b : bytes) c = table[(c ^ b) & 255] ^ (c >> 8);
  return ~c;
}
void u32(std::string& out, uint32_t v) {
  for (int i = 0; i < 4; ++i) out.push_back(static_cast<char>(v >> (8 * i)));
}
void u64(std::string& out, uint64_t v) {
  for (int i = 0; i < 8; ++i) out.push_back(static_cast<char>(v >> (8 * i)));
}
void string(std::string& out, std::string_view v) {
  if (v.size() > kMaxFrame) throw std::runtime_error("disk string too large");
  u32(out, static_cast<uint32_t>(v.size()));
  out.append(v);
}
struct Reader {
  std::string_view in;
  std::string_view take(size_t n) {
    if (n > in.size()) throw std::runtime_error("truncated disk record");
    auto result = in.substr(0, n);
    in.remove_prefix(n);
    return result;
  }
  uint64_t integer(size_t n) {
    auto v = take(n);
    uint64_t result = 0;
    for (size_t i = 0; i < n; ++i) result |= uint64_t(static_cast<unsigned char>(v[i])) << (8 * i);
    return result;
  }
  uint32_t u32() { return static_cast<uint32_t>(integer(4)); }
  uint64_t u64() { return integer(8); }
  int64_t id(bool allow_zero = false) {
    uint64_t v = u64();
    if (v > INT64_MAX || (!v && !allow_zero)) throw std::runtime_error("invalid disk id");
    return static_cast<int64_t>(v);
  }
  std::string string() { return std::string(take(u32())); }
};
struct Header { uint64_t sequence; uint32_t length, crc; };
Header header(std::string_view bytes, std::string_view magic) {
  if (bytes.size() != kHeader || bytes.substr(0, 8) != magic)
    throw std::runtime_error("invalid disk magic/header");
  Reader r{bytes.substr(8)};
  Header h{r.u64(), r.u32(), r.u32()};
  if (r.u32() != crc32c(bytes.substr(0, 24)) || h.length > kMaxFrame)
    throw std::runtime_error("invalid disk header checksum/length");
  return h;
}
std::string frame(std::string_view magic, uint64_t sequence, std::string_view payload) {
  if (payload.size() > kMaxFrame) throw std::runtime_error("commit batch too large");
  std::string out;
  out.reserve(kHeader + payload.size() + kFooter);
  out.append(magic);
  u64(out, sequence);
  u32(out, static_cast<uint32_t>(payload.size()));
  u32(out, crc32c(payload));
  u32(out, crc32c(out));
  out.append(payload);
  out.append(kDone);
  return out;
}
void verify(const Header& h, std::string_view payload, std::string_view footer) {
  if (payload.size() != h.length || crc32c(payload) != h.crc || footer != kDone)
    throw std::runtime_error("disk payload checksum/commit marker mismatch");
}
void post(std::string& out, const Post& p) {
  u64(out, static_cast<uint64_t>(p.id));
  string(out, p.created_at);
  string(out, p.prefix);
}
Post post(Reader& r) {
  int64_t id = r.id();
  auto timestamp = r.string(), prefix = r.string();
  JType type;
  JField f[] = {{"id"}, {"body"}, {"created_at"}, {"author"}, {"like_count"}};
  std::string json = prefix + "0}";
  int64_t json_id = 0;
  if (!JsonParser(json).parse(&type, f, 5) || type != J_OBJ || f[0].type != J_NUM ||
      parse_id(f[0].raw, &json_id) != 1 || json_id != id || f[1].type != J_STR ||
      f[1].str.empty() || utf8_length(f[1].str) > 500 || f[2].type != J_STR ||
      f[2].str != timestamp || !valid_timestamp(timestamp) || f[3].type != J_STR)
    throw std::runtime_error("invalid persisted post");
  Post p(id, f[1].str, f[3].str, std::move(timestamp));
  if (p.prefix != prefix) throw std::runtime_error("invalid persisted post encoding");
  return p;
}
std::string snapshot(const Store& store) {
  std::string out;
  u64(out, static_cast<uint64_t>(store.next_id()));
  u32(out, static_cast<uint32_t>(store.size()));
  for (size_t i = 0; i < store.size(); ++i) {
    const auto& p = store.oldest(i);
    post(out, p);
    u64(out, static_cast<uint64_t>(p.likes.size()));  // Persist the precomputed counter.
    for (uint64_t word : p.likes.dense()) u64(out, word);
    u32(out, static_cast<uint32_t>(p.likes.sparse().size()));
    for (int64_t user : p.likes.sparse()) u64(out, static_cast<uint64_t>(user));
  }
  return out;
}
Store snapshot(std::string_view bytes) {
  Reader r{bytes};
  Store result;
  int64_t next = r.id(true);
  unsigned count = r.u32();
  if (count > Store::kCapacity) throw std::runtime_error("oversized snapshot window");
  for (unsigned i = 0; i < count; ++i) {
    Post p = post(r);
    if (next && p.id >= next) throw std::runtime_error("invalid snapshot next id");
    int64_t likes = r.id(true);
    std::array<uint64_t, LikeSet::kDenseUsers / 64> dense{};
    for (auto& word : dense) word = r.u64();
    unsigned n = r.u32();
    if (n > r.in.size() / 8) throw std::runtime_error("truncated snapshot likes");
    std::unordered_set<int64_t> sparse;
    sparse.reserve(n);
    for (unsigned j = 0; j < n; ++j)
      if (!sparse.insert(r.id()).second) throw std::runtime_error("duplicate snapshot like");
    p.likes.restore(dense, std::move(sparse), likes);
    result.restore_post(std::move(p));
  }
  if (!r.in.empty()) throw std::runtime_error("trailing snapshot data");
  result.restore_next_id(next);
  return result;
}
void append_post(std::string& out, const Post& p) {
  out.push_back(1);
  post(out, p);
}
void append_like(std::string& out, int64_t id, int64_t user, int64_t count) {
  out.push_back(2);
  u64(out, static_cast<uint64_t>(id));
  u64(out, static_cast<uint64_t>(user));
  u64(out, static_cast<uint64_t>(count));
}
void replay(Store& store, std::string_view bytes) {
  Reader r{bytes};
  Store candidate = store;
  while (!r.in.empty()) {
    auto tag = r.integer(1);
    if (tag == 1) candidate.replay_post(post(r));
    else if (tag == 2) {
      int64_t id = r.id(), user = r.id(), count = r.id();
      if (candidate.like(id, user) != 1 || candidate.find(id)->likes.size() != count)
        throw std::runtime_error("WAL like membership/count mismatch");
    } else throw std::runtime_error("unknown WAL operation");
  }
  store = std::move(candidate);  // A group is applied in its entirety.
}
void fail(const char* operation) { throw std::runtime_error(std::string(operation) + ": " + strerror(errno)); }
void sync(int fd) {
  int rc;
#ifdef __APPLE__
  do { rc = fsync(fd); } while (rc && errno == EINTR);
#else
  do { rc = fdatasync(fd); } while (rc && errno == EINTR);
#endif
  if (rc) fail("sync");
}
void write_at(int fd, std::string_view bytes, off_t offset) {
  while (!bytes.empty()) {
    ssize_t n = pwrite(fd, bytes.data(), bytes.size(), offset);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { if (!n) errno = EIO; fail("pwrite"); }
    offset += n;
    bytes.remove_prefix(static_cast<size_t>(n));
  }
}
std::string read_at(int fd, size_t size, off_t offset) {
  std::string result(size, '\0');
  size_t done = 0;
  while (done < size) {
    ssize_t n = pread(fd, result.data() + done, size - done, offset + done);
    if (n < 0 && errno == EINTR) continue;
    if (n <= 0) { if (!n) errno = EIO; fail("pread"); }
    done += static_cast<size_t>(n);
  }
  return result;
}
struct Fd {
  int value = -1;
  ~Fd() { if (value >= 0) close(value); }
};
}  // namespace disk

class Journal {
 public:
  Journal() = default;
  Journal(const Journal&) = delete;
  Journal& operator=(const Journal&) = delete;
  int fd() const { return wal_.value; }
  off_t offset() const { return offset_; }
  uint64_t sequence() const { return sequence_; }
  uint64_t checkpoint_bytes = 64 * 1024 * 1024;

  void open(const std::string& directory, const char* seed, Store& store) {
    if (fd() >= 0) throw std::runtime_error("journal already open");
    bool created = std::filesystem::create_directories(directory);
    if (created) {
      // fsync(directory) alone does not persist the directory's entry in its parent.
      // Sync the ancestor chain too, including parents created by create_directories.
      auto parent = std::filesystem::absolute(directory).lexically_normal().parent_path();
      for (;;) {
        disk::Fd ancestor{::open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC)};
        if (ancestor.value < 0) disk::fail("open parent directory");
        int rc;
        do { rc = fsync(ancestor.value); } while (rc && errno == EINTR);
        if (rc) disk::fail("sync parent directory");
        if (parent == parent.root_path()) break;
        parent = parent.parent_path();
      }
    }
    directory_ = directory;
    dir_.value = ::open(directory.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (dir_.value < 0) disk::fail("open store directory");
    lock_.value = ::open(path("writer.lock").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (lock_.value < 0 || flock(lock_.value, LOCK_EX | LOCK_NB)) disk::fail("lock store (single writer required)");
    disk::Fd file{::open(path("snapshot.bin").c_str(), O_RDONLY | O_CLOEXEC)};
    if (file.value >= 0) {
      struct stat info{};
      if (fstat(file.value, &info)) disk::fail("stat snapshot");
      if (info.st_size < static_cast<off_t>(disk::kHeader + disk::kFooter) ||
          info.st_size > static_cast<off_t>(disk::kMaxFrame + disk::kHeader + disk::kFooter))
        throw std::runtime_error("invalid snapshot size");
      auto bytes = disk::read_at(file.value, static_cast<size_t>(info.st_size), 0);
      auto h = disk::header(std::string_view(bytes).substr(0, disk::kHeader), disk::kSnapshotMagic);
      disk::verify(h, std::string_view(bytes).substr(disk::kHeader, bytes.size() - disk::kHeader - disk::kFooter),
                   std::string_view(bytes).substr(bytes.size() - disk::kFooter));
      store = disk::snapshot(std::string_view(bytes).substr(disk::kHeader, h.length));
      sequence_ = h.sequence;
    } else {
      if (errno != ENOENT) disk::fail("open snapshot");
      struct stat info{};
      int rc = stat(path("journal.wal").c_str(), &info);
      if (!rc && info.st_size) throw std::runtime_error("WAL exists without its initial snapshot");
      if (rc && errno != ENOENT) disk::fail("stat initial WAL");
      Store initial;
      if (seed && *seed) initial.load_seed(seed);
      store = std::move(initial);
      write_snapshot(store);
    }
    wal_.value = ::open(path("journal.wal").c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0644);
    if (fd() < 0) disk::fail("open WAL");
    sync_directory();  // Make a newly created WAL name durable before accepting writes.
    recover(store);
  }
  std::string prepare(std::string_view operations) const {
    if (sequence_ == UINT64_MAX) throw std::overflow_error("WAL sequence exhausted");
    return disk::frame(disk::kWalMagic, sequence_ + 1, operations);
  }
  void published(size_t bytes) {
    offset_ += static_cast<off_t>(bytes);
    ++sequence_;
  }
  void rollback_uncommitted() {
    if (ftruncate(fd(), offset_)) disk::fail("truncate failed commit");
    disk::sync(fd());
  }
  bool needs_checkpoint() const { return static_cast<uint64_t>(offset_) >= checkpoint_bytes; }
  void checkpoint(const Store& store) {
    // Never truncate before the renamed snapshot AND its directory entry are durable.
    write_snapshot(store);
    if (ftruncate(fd(), 0)) disk::fail("truncate checkpointed WAL");
    disk::sync(fd());
    offset_ = 0;
  }
 private:
  disk::Fd dir_, lock_, wal_;
  std::string directory_;
  off_t offset_ = 0;
  uint64_t sequence_ = 0;
  std::string path(const char* name) const { return directory_ + "/" + name; }
  void sync_directory() {
    int rc;
    do { rc = fsync(dir_.value); } while (rc && errno == EINTR);
    if (rc) disk::fail("sync store directory");
  }
  void write_snapshot(const Store& store) {
    auto bytes = disk::frame(disk::kSnapshotMagic, sequence_, disk::snapshot(store));
    disk::Fd tmp{::open(path("snapshot.tmp").c_str(), O_RDWR | O_CREAT | O_TRUNC | O_CLOEXEC, 0644)};
    if (tmp.value < 0) disk::fail("open snapshot temporary file");
    disk::write_at(tmp.value, bytes, 0);
    disk::sync(tmp.value);
    if (rename(path("snapshot.tmp").c_str(), path("snapshot.bin").c_str())) disk::fail("publish snapshot");
    sync_directory();
  }
  void recover(Store& store) {
    struct stat info{};
    if (fstat(fd(), &info)) disk::fail("stat WAL");
    off_t position = 0;
    const uint64_t snapshot_sequence = sequence_;
    uint64_t previous = 0;
    bool have_previous = false;
    while (position < info.st_size) {
      if (info.st_size - position < static_cast<off_t>(disk::kHeader)) break;
      auto raw_header = disk::read_at(fd(), disk::kHeader, position);
      auto h = disk::header(raw_header, disk::kWalMagic);
      if (!h.sequence || (have_previous && (previous == UINT64_MAX || h.sequence != previous + 1)))
        throw std::runtime_error("WAL sequence gap");
      size_t size = disk::kHeader + h.length + disk::kFooter;
      if (info.st_size - position < static_cast<off_t>(size)) break;
      auto tail = disk::read_at(fd(), h.length + disk::kFooter, position + disk::kHeader);
      disk::verify(h, std::string_view(tail).substr(0, h.length), std::string_view(tail).substr(h.length));
      if (h.sequence > sequence_) {
        if (sequence_ == UINT64_MAX || h.sequence != sequence_ + 1)
          throw std::runtime_error("WAL does not follow snapshot sequence");
        disk::replay(store, std::string_view(tail).substr(0, h.length));
        sequence_ = h.sequence;
      }
      previous = h.sequence;
      have_previous = true;
      position += static_cast<off_t>(size);
    }
    // Retire a log fully covered by the snapshot, including the rename/truncate crash
    // window. This also lets the next frame begin directly at the snapshot's sequence.
    offset_ = sequence_ == snapshot_sequence ? 0 : position;
    if (offset_ != info.st_size) rollback_uncommitted();
    // A process crash may leave a complete, unacknowledged frame. Sync replay before
    // exposing it: clients can retry likes safely, while post creation remains non-idempotent.
    disk::sync(fd());
  }
};

}  // namespace top20
