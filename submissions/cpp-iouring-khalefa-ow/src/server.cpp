// Linux 6.0+: one io_uring issuer, pooled receive buffers, and SQLite group commit.
#include <arpa/inet.h>
#include <liburing.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <signal.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>
#include <thread>
#include "batch.hpp"

namespace {
using namespace challenge;

constexpr unsigned kQueueDepth = 2048;
constexpr unsigned kBuffers = 2048;  // 8 MiB shared by ALL connections, including idle ones.
constexpr unsigned kCompletions = 256;
constexpr unsigned kBufferGroup = 1;
constexpr size_t kMaxInput = kMaxHeaderBytes + kMaxBodyBytes + 4096;
constexpr size_t kMaxOutput = 64 * 1024;

struct Conn;
enum class OpKind { Accept, Receive, Send, Timer, Cancel };
struct Op { OpKind kind; Conn* conn = nullptr; };
struct Conn {
  explicit Conn(int socket) : fd(socket), recv_op{OpKind::Receive, this},
                            send_op{OpKind::Send, this} {}
  int fd;
  bool inflight = false, closing = false, close_after = false, pending_write = false;
  int64_t last_active = 0;
  size_t sent = 0;
  std::string in, out;
  Write write;
  Op recv_op, send_op;
};

io_uring ring;
io_uring_buf_ring* buffers;
char* receive_memory;
std::vector<Conn*> connections;
std::vector<Conn*> pending;
std::vector<Conn*> batch_connections;
std::vector<Write*> write_args;
std::string response_body, chunked_body;
Op accept_op{OpKind::Accept}, timer_op{OpKind::Timer}, cancel_op{OpKind::Cancel};
__kernel_timespec tick{1, 0};
bool accept_active = false, timer_active = false;
int listener;
volatile sig_atomic_t stopping = 0;
std::atomic<bool> checkpoint_stop{false};
int64_t now_s;
GroupCommit* committer;

void signal_stop(int) { stopping = 1; }

io_uring_sqe* next_sqe() {
  auto* sqe = io_uring_get_sqe(&ring);
  if (!sqe) {
    int rc;
    do { rc = io_uring_submit(&ring); } while (rc == -EINTR);
    if (rc < 0) die("io_uring_submit", strerror(-rc));
    sqe = io_uring_get_sqe(&ring);
    if (!sqe) die("io_uring submission queue stayed full");
  }
  return sqe;
}

void release_conn(Conn* c) {
  // A CQE carries a pointer into Conn: never free it while an operation or write owns it.
  if (c->inflight || c->pending_write) return;
  connections[c->fd] = nullptr;
  close(c->fd);
  delete c;
}

void close_conn(Conn* c) {
  if (!c->closing) {
    c->closing = true;
    // This wakes a pending socket receive/send without recycling its fd or CQE pointer.
    shutdown(c->fd, SHUT_RDWR);
  }
  release_conn(c);
}

void queue_accept() {
  auto* sqe = next_sqe();
  io_uring_prep_multishot_accept(sqe, listener, nullptr, nullptr, SOCK_CLOEXEC);
  io_uring_sqe_set_data(sqe, &accept_op);
  accept_active = true;
}

void queue_tick() {
  auto* sqe = next_sqe();
  io_uring_prep_timeout(sqe, &tick, 0, 0);
  io_uring_sqe_set_data(sqe, &timer_op);
  timer_active = true;
}

void queue_receive(Conn* c) {
  auto* sqe = next_sqe();
  io_uring_prep_recv(sqe, c->fd, nullptr, kReadChunk, 0);
  sqe->flags |= IOSQE_BUFFER_SELECT;
  sqe->buf_group = kBufferGroup;
  sqe->ioprio |= IORING_RECVSEND_POLL_FIRST;
  io_uring_sqe_set_data(sqe, &c->recv_op);
  c->inflight = true;
}

void queue_send(Conn* c) {
  auto* sqe = next_sqe();
  io_uring_prep_send(sqe, c->fd, c->out.data() + c->sent,
                     c->out.size() - c->sent, MSG_NOSIGNAL);
  io_uring_sqe_set_data(sqe, &c->send_op);
  c->inflight = true;
}

// Parse in the selected shared buffer when possible, copying only partial/pipelined tails.
// Writes stop the pipeline until their commit, preserving response and request ordering.
size_t serve(Conn* c, const char* data, size_t len) {
  size_t offset = 0;
  unsigned count = 0;
  while (offset < len && !c->close_after && !c->pending_write &&
         c->out.size() < kMaxOutput && count++ < 32) {
    Request request;
    size_t used = 0;
    Parse parsed = parse_request(data + offset, len - offset, &request, &used, chunked_body);
    if (parsed == Parse::kIncomplete) break;
    response_body.clear();
    if (parsed != Parse::kOk) {
      c->close_after = true;
      int status = parsed == Parse::kHeadersTooLarge ? 431 : parsed == Parse::kBodyTooLarge ? 413 : 400;
      error(response_body, status, status == 400 ? "bad request" : status == 413 ? "payload too large"
                                                                                 : "headers too large");
      append_response(c->out, status, response_body, true);
      return len;
    }
    int status = dispatch(request, response_body, c->write);
    c->close_after = !request.keep_alive;
    offset += used;
    if (!status) {
      c->pending_write = true;
      pending.push_back(c);
    } else {
      append_response(c->out, status, response_body, c->close_after);
    }
  }
  return offset;
}

void start_io(Conn* c) {
  if (c->closing || stopping) { close_conn(c); return; }
  if (c->pending_write || c->inflight) return;
  if (!c->out.empty()) { queue_send(c); return; }
  if (c->close_after) { close_conn(c); return; }
  if (!c->in.empty()) {
    size_t used = serve(c, c->in.data(), c->in.size());
    c->in.erase(0, used);
    if (c->pending_write) return;
    if (!c->out.empty()) { queue_send(c); return; }
  }
  queue_receive(c);
}

void commit_pending() {
  // start_io() can stage the next pipelined write, so detach this group before resuming clients.
  batch_connections.clear();
  batch_connections.swap(pending);
  write_args.clear();
  for (Conn* c : batch_connections) write_args.push_back(&c->write);
  committer->run(write_args);
  for (Conn* c : batch_connections) {
    c->pending_write = false;
    if (c->closing || stopping) { close_conn(c); continue; }
    append_response(c->out, c->write.status, c->write.response, c->close_after);
    c->write.response.clear();
    c->write.text.clear();
    c->write.username.clear();
    start_io(c);  // The COMMIT above has finished; success bytes can now enter the SQ.
  }
  batch_connections.clear();
}

void receive_done(Conn* c, int result, unsigned flags) {
  c->inflight = false;
  bool selected = flags & IORING_CQE_F_BUFFER;
  unsigned id = flags >> IORING_CQE_BUFFER_SHIFT;
  if (selected && id >= kBuffers) die("invalid receive buffer id");
  if (!c->closing && !stopping && result > 0) {
    if (!selected || static_cast<size_t>(result) > kReadChunk) die("receive without valid buffer");
    c->last_active = now_s;
    const char* data = receive_memory + id * kReadChunk;
    if (c->in.empty()) {
      size_t used = serve(c, data, result);
      if (used < static_cast<size_t>(result)) c->in.assign(data + used, result - used);
    } else {
      c->in.append(data, result);
      size_t used = serve(c, c->in.data(), c->in.size());
      c->in.erase(0, used);
    }
  }
  // Publish the returned buffer only after every view into it has been consumed/copied.
  if (selected) {
    io_uring_buf_ring_add(buffers, receive_memory + id * kReadChunk, kReadChunk, id,
                          io_uring_buf_ring_mask(kBuffers), 0);
    io_uring_buf_ring_advance(buffers, 1);
  }
  if (c->closing || stopping) { close_conn(c); return; }
  // More ready sockets than available buffers is transient. Re-arm after recycling CQ buffers.
  if (result == -ENOBUFS || result == -EINTR || result == -EAGAIN) { start_io(c); return; }
  if (result <= 0) { close_conn(c); return; }
  if (c->in.size() > kMaxInput) {
    response_body.clear();
    error(response_body, 413, "payload too large");
    append_response(c->out, 413, response_body, true);
    c->close_after = true;
    c->in.clear();
  }
  start_io(c);
}

void send_done(Conn* c, int result) {
  c->inflight = false;
  if (c->closing || stopping) { close_conn(c); return; }
  if (result == -EINTR || result == -EAGAIN) { queue_send(c); return; }
  if (result <= 0) { close_conn(c); return; }
  c->last_active = now_s;
  c->sent += result;
  if (c->sent < c->out.size()) { queue_send(c); return; }
  c->sent = 0;
  c->out.clear();
  if (c->out.capacity() > 2 * kMaxOutput) std::string().swap(c->out);
  if (c->in.empty() && c->in.capacity() > kMaxHeaderBytes) std::string().swap(c->in);
  start_io(c);
}

void accept_done(int result, unsigned flags) {
  accept_active = flags & IORING_CQE_F_MORE;
  if (result >= 0) {
    if (stopping) { close(result); return; }
    int one = 1;
    setsockopt(result, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    if (static_cast<size_t>(result) >= connections.size()) connections.resize(result * 2 + 1, nullptr);
    auto* c = new Conn(result);
    c->last_active = now_s;
    connections[result] = c;
    queue_receive(c);
  } else if (result == -EINVAL || result == -EOPNOTSUPP) {
    die("multishot accept unavailable: use Linux 6.0 or newer", strerror(-result));
  }
  // On fd/memory pressure the timer retries, avoiding a busy loop.
  if (!accept_active && !stopping && result != -EMFILE && result != -ENFILE && result != -ENOBUFS &&
      result != -ENOMEM) queue_accept();
}

void process_completions() {
  struct Completion { Op* op; int result; unsigned flags; } ready[kCompletions];
  io_uring_cqe* entries[kCompletions];
  unsigned n = io_uring_peek_batch_cqe(&ring, entries, kCompletions);
  for (unsigned i = 0; i < n; ++i)
    ready[i] = {static_cast<Op*>(io_uring_cqe_get_data(entries[i])), entries[i]->res, entries[i]->flags};
  io_uring_cq_advance(&ring, n);  // Copies stay valid even if next_sqe() submits more work.
  now_s = monotonic_s();
  for (unsigned i = 0; i < n; ++i) {
    auto e = ready[i];
    switch (e.op->kind) {
      case OpKind::Accept: accept_done(e.result, e.flags); break;
      case OpKind::Receive: receive_done(e.op->conn, e.result, e.flags); break;
      case OpKind::Send: send_done(e.op->conn, e.result); break;
      case OpKind::Cancel: break;
      case OpKind::Timer:
        timer_active = false;
        if (!stopping) {
          for (Conn* c : connections)
            if (c && !c->closing && now_s - c->last_active > kIdleTimeoutS) close_conn(c);
          if (!accept_active) queue_accept();
          queue_tick();
        }
        break;
    }
  }
  if (!pending.empty()) commit_pending();
}

int listen_on(const char* host, const char* port) {
  addrinfo hints{}, *addresses = nullptr;
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  hints.ai_flags = AI_PASSIVE | AI_NUMERICSERV;
  int rc = getaddrinfo(host, port, &hints, &addresses);
  if (rc) die("getaddrinfo", gai_strerror(rc));
  int fd = socket(addresses->ai_family, SOCK_STREAM | SOCK_CLOEXEC, 0);
  if (fd < 0) die("socket", strerror(errno));
  int one = 1;
  setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
  if (bind(fd, addresses->ai_addr, addresses->ai_addrlen) < 0) die("bind", strerror(errno));
  freeaddrinfo(addresses);
  if (listen(fd, 65535) < 0) die("listen", strerror(errno));
  return fd;  // Blocking sockets: io_uring polls them asynchronously.
}

void init_ring() {
  io_uring_params params{};
  params.flags = IORING_SETUP_SINGLE_ISSUER | IORING_SETUP_COOP_TASKRUN | IORING_SETUP_CQSIZE;
  // Accommodate a simultaneous completion burst across the full 65,535 fd budget.
  // This costs 1 MiB and prevents an accept/receive burst from crowding out completions.
  params.cq_entries = 65536;
  int rc = io_uring_queue_init_params(kQueueDepth, &ring, &params);
  if (rc < 0) die("io_uring setup (requires Linux 6.0+ and permitted io_uring syscalls)", strerror(-rc));
  if (!(params.features & IORING_FEAT_NODROP)) die("io_uring requires IORING_FEAT_NODROP");
  int error_code;
  buffers = io_uring_setup_buf_ring(&ring, kBuffers, kBufferGroup, 0, &error_code);
  if (!buffers) die("io_uring provided buffer ring", strerror(-error_code));
  receive_memory = static_cast<char*>(mmap(nullptr, kBuffers * kReadChunk,
                      PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0));
  if (receive_memory == MAP_FAILED) die("receive buffer allocation", strerror(errno));
  for (unsigned i = 0; i < kBuffers; ++i)
    io_uring_buf_ring_add(buffers, receive_memory + i * kReadChunk, kReadChunk, i,
                          io_uring_buf_ring_mask(kBuffers), i);
  io_uring_buf_ring_advance(buffers, kBuffers);
}

void checkpoint_worker(std::string path) {
  auto* db = open_db(path.c_str());
  while (!checkpoint_stop.load(std::memory_order_relaxed)) {
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    sqlite3_wal_checkpoint_v2(db, nullptr, SQLITE_CHECKPOINT_PASSIVE, nullptr, nullptr);
  }
  sqlite3_close(db);
}

void cancel(Op* op) {
  auto* sqe = next_sqe();
  io_uring_prep_cancel(sqe, op, 0);
  io_uring_sqe_set_data(sqe, &cancel_op);
}

void drain_and_close() {
  for (Conn* c : connections) if (c) close_conn(c);
  if (accept_active) cancel(&accept_op);
  if (timer_active) cancel(&timer_op);
  // Consume all CQEs before freeing any kernel-owned buffer. Shutdown wakes socket operations.
  for (;;) {
    bool live = accept_active || timer_active;
    for (Conn* c : connections) if (c) { live = true; break; }
    if (!live) break;
    int rc = io_uring_submit_and_wait(&ring, 1);
    if (rc == -EINTR) continue;
    if (rc == -EBUSY) { process_completions(); continue; }
    if (rc < 0) die("io_uring shutdown", strerror(-rc));
    process_completions();
  }
  io_uring_free_buf_ring(&ring, buffers, kBuffers, kBufferGroup);
  io_uring_queue_exit(&ring);
  munmap(receive_memory, kBuffers * kReadChunk);
  close(listener);
}
}  // namespace

int main() {
  const char* path = std::getenv("SQLITE_PATH");
  const char* secret = std::getenv("JWT_SECRET");
  const char* host = std::getenv("HOST");
  const char* port = std::getenv("PORT");
  if (!path || !*path) die("SQLITE_PATH is not set");
  if (!secret) die("JWT_SECRET is not set");
  if (!host || !*host) host = "0.0.0.0";
  if (!port || !*port) port = "80";
  signal(SIGPIPE, SIG_IGN);
  struct sigaction action{};
  action.sa_handler = signal_stop;
  sigemptyset(&action.sa_mask);
  sigaction(SIGINT, &action, nullptr);
  sigaction(SIGTERM, &action, nullptr);
  rlimit limit{};
  if (!getrlimit(RLIMIT_NOFILE, &limit) && limit.rlim_cur < limit.rlim_max) {
    limit.rlim_cur = limit.rlim_max;
    setrlimit(RLIMIT_NOFILE, &limit);
  }
  g_start_s = now_s = monotonic_s();
  hmac_init(secret);
  init_db(path);
  connections.resize(4096, nullptr);
  pending.reserve(kCompletions);
  batch_connections.reserve(kCompletions);
  write_args.reserve(kCompletions);
  response_body.reserve(16 * 1024);
  init_ring();
  listener = listen_on(host, port);
  std::thread checkpoints(checkpoint_worker, std::string(path));
  {
    GroupCommit group;
    committer = &group;
    queue_accept();
    queue_tick();
    std::fprintf(stderr, "io_uring listening on %s:%s; shared recv pool: 8 MiB; group limit: 256\n", host, port);
    while (!stopping) {
      int rc = io_uring_submit_and_wait(&ring, 1);
      if (rc == -EINTR) continue;
      if (rc == -EBUSY) { process_completions(); continue; }
      if (rc < 0) die("io_uring_submit_and_wait", strerror(-rc));
      process_completions();
    }
    drain_and_close();
    std::fprintf(stderr, "group commit: %llu batches, %llu writes, largest %llu, failed %llu\n",
                 static_cast<unsigned long long>(group.batches),
                 static_cast<unsigned long long>(group.requests),
                 static_cast<unsigned long long>(group.largest),
                 static_cast<unsigned long long>(group.failures));
  }
  checkpoint_stop.store(true, std::memory_order_relaxed);
  checkpoints.join();
  close_db();
}
