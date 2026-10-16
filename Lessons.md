Profile-Guided Optimization (PGO), the concept behaves as follows:


`io_uring` provides many optimizations beyond multishot accept, direct sockets, and provided-buffer rings. Some reduce system calls, others reduce CPU overhead, memory allocation, network latency, or context switching.

For a high-performance TCP server, I would divide them into six categories.

## 1. Most important `io_uring` optimizations

| Feature                      | Purpose                                 | Potential benefit                                    |
| ---------------------------- | --------------------------------------- | ---------------------------------------------------- |
| `IORING_SETUP_SQPOLL`        | Kernel thread polls submission queue    | Submit I/O without a syscall on the normal fast path |
| `IORING_SETUP_SINGLE_ISSUER` | Only one submitting task per ring       | Less synchronization and simpler ownership           |
| `IORING_SETUP_DEFER_TASKRUN` | Defer completion-related task work      | Better batching and event-loop control               |
| `IORING_SETUP_COOP_TASKRUN`  | Cooperative task-work processing        | Fewer unnecessary interrupts                         |
| `IORING_REGISTER_NAPI`       | Busy-poll network processing            | Lower network latency                                |
| `IORING_OP_SEND_ZC`          | Zero-copy socket transmission           | Reduce payload copying                               |
| `IORING_OP_SENDMSG_ZC`       | Zero-copy scatter/gather sending        | Efficient transmission from multiple buffers         |
| `IORING_OP_SEND_BUNDLE`      | Send from multiple provided buffers     | Reduce repeated send submissions                     |
| `IORING_OP_RECV_ZC`          | Specialized zero-copy network receiving | Reduce receive-side copies where supported           |
| `IOSQE_IO_LINK`              | Chain dependent operations              | Reduce user/kernel coordination                      |
| `IORING_REGISTER_BUFFERS`    | Register fixed application buffers      | Avoid repeated buffer mapping overhead               |
| `IORING_REGISTER_RING_FDS`   | Register ring descriptors               | Reduce ring descriptor lookup overhead               |
| `IORING_SETUP_CQSIZE`        | Customize completion queue capacity     | Reduce completion queue pressure                     |

These are different kinds of features: setup flags, registration operations, submission flags, and actual I/O operations. They are not all interchangeable or universally beneficial.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+2



## 2. Submission and completion optimizations

### A. SQPOLL — kernel polls the submission queue

Without SQPOLL:

```
Application submits SQEs
         ↓
io_uring_enter() syscall
         ↓
Kernel processes requests
```

With SQPOLL:

```
Application writes SQEs
         ↓
Shared submission queue
         ↓
Kernel SQPOLL thread
         ↓
Kernel processes requests
```

The kernel thread watches for submissions, avoiding submission syscalls while the polling thread is active.

Tradeoff: It can consume significant CPU resources. If the thread goes idle and sleeps, the application may need a syscall to wake it.

### B. SINGLE_ISSUER + DEFER_TASKRUN

This is especially interesting for a single-threaded event loop.

```
struct io_uring_params p = {0};

p.flags = IORING_SETUP_SINGLE_ISSUER |
          IORING_SETUP_DEFER_TASKRUN;

io_uring_queue_init_params(4096, &ring, &p);
```

The application controls when deferred completion work is processed, potentially improving batching and reducing interruptions.

This combination is worth benchmarking as a baseline before enabling SQPOLL. `DEFER_TASKRUN` requires careful event-loop design because the application must regularly enter the kernel to make progress on deferred work.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://man7.org\&sz=32)

Linux manual page

+1



### C. Batched submission and completion

Instead of processing one operation at a time, submit many SQEs and drain multiple CQEs together.

For example:

```
io_uring_submit(&ring);

unsigned count = io_uring_peek_batch_cqe(
    &ring, cqes, 64
);
```

This can improve efficiency by amortizing per-batch overhead and improving cache locality.

Unlike SQPOLL, batching doesn't require a dedicated polling thread.

## 3. Network I/O optimizations

These are particularly relevant when comparing `io_uring` with an `epoll`-based TCP server.

Zero-copy send — `IORING_OP_SEND_ZC`

Avoids some copying from application memory to the networking stack. Useful for larger responses and high-throughput servers, but additional completion notifications are needed before reusing memory.

Bundled send/recv — `IORING_RECVSEND_BUNDLE`

Allows one operation to consume multiple buffers from a provided-buffer group. Particularly useful for streaming and for reducing repeated interactions with the TCP stack.

NAPI busy polling — `IORING_REGISTER_NAPI`

Polls the network receive path instead of relying entirely on interrupts. It can reduce latency for busy network services at the cost of increased CPU usage.

Incremental buffer consumption — `IOU_PBUF_RING_INC`

Allows a buffer to be consumed across multiple completions instead of treating every partial receive as consuming an entire buffer. Supported starting with Linux 6.12.

Bundled I/O and incremental buffer consumption are especially interesting when the workload contains messages of widely varying sizes.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+2



## 4. Other useful options

| Feature                        | When it helps                                                   |
| ------------------------------ | --------------------------------------------------------------- |
| `IOSQE_CQE_SKIP_SUCCESS`       | Suppress unnecessary successful completion entries              |
| `IOSQE_IO_LINK`                | Execute a sequence of dependent I/O operations                  |
| `IORING_OP_LINK_TIMEOUT`       | Attach a timeout to an operation chain                          |
| `IORING_OP_ASYNC_CANCEL`       | Cancel outstanding I/O requests                                 |
| `IORING_OP_MSG_RING`           | Communicate between separate `io_uring` instances               |
| `IORING_SETUP_ATTACH_WQ`       | Share asynchronous worker resources across rings                |
| `IORING_SETUP_NO_SQARRAY`      | Reduce submission queue indirection                             |
| `IORING_SETUP_IOPOLL`          | Poll supported storage devices instead of relying on interrupts |
| `IORING_REGISTER_BUFFERS`      | Use fixed registered memory for supported I/O                   |
| `IORING_REGISTER_FILES_UPDATE` | Modify registered file slots dynamically                        |

For storage-heavy applications, `IOPOLL`, fixed buffers, and registered files can be particularly valuable. `IOPOLL` targets compatible storage I/O, not ordinary TCP receive polling.&#x20;

[image](https://www.google.com/s2/favicons?domain=https://github.com\&sz=32)

GitHub

+2



## 5. Which combination should you use for a fast TCP server?

I'd benchmark these configurations incrementally rather than turning everything on at once.

| Configuration            | Features                             | Main experiment             |
| ------------------------ | ------------------------------------ | --------------------------- |
| A — Baseline             | Ordinary `io_uring` send/recv        | Establish performance       |
| B — Multishot            | Multishot accept and recv, PBUF ring | Reduce submissions          |
| C — Direct               | B + registered socket table          | Reduce FD overhead          |
| D — Event-loop optimized | C + SINGLE_ISSUER + DEFER_TASKRUN    | Reduce task-work overhead   |
| E — Polling              | C + SQPOLL                           | Reduce submission syscalls  |
| F — Network optimized    | D + NAPI busy polling                | Test latency vs CPU         |
| G — Send optimized       | D + send bundles or SEND_ZC          | Improve outbound throughput |

For comparisons, I would track requests/second, throughput, p50/p99 latency, CPU utilization, context switches, syscall count, and memory usage. Run equivalent workloads at different connection counts and message sizes.

My starting point would be D, assuming each worker thread owns its own ring. Then test SQPOLL, NAPI, and zero-copy sends individually. SQPOLL and DEFER_TASKRUN interact with event-loop scheduling, so measuring them separately matters.

## 6. Kernel compatibility

Feature support depends on the running Linux kernel and sometimes the network driver or hardware. For reference:

- Linux 6.0: `IORING_SETUP_SINGLE_ISSUER`
- Linux 6.1: `IORING_SETUP_DEFER_TASKRUN`
- Linux 6.9: NAPI registration
- Linux 6.10: Bundled send/recv support
- Linux 6.12: Incremental provided-buffer consumption

You can inspect the running kernel with `uname -r` and use `io_uring_get_probe()` or `io_uring_opcode_supported()` for opcode support. Feature-specific setup and registration still need to be checked separately.

Official references: [liburing source and examples](https://github.com/axboe/liburing?utm_source=chatgpt.com) and [io_uring manual](https://man7.org/linux/man-pages/man7/io_uring.7.html?utm_source=chatgpt.com).

The most interesting technical question is whether, after multishot receive and direct sockets, the dominant overhead moves away from system calls and into TCP processing, cache behavior, buffer lifecycle management, and application-level parsing. That is where SQPOLL, NAPI, batching, and bundled I/O can produce very different results depending on the workload.