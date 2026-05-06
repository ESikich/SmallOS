# Socket Subsystem Rollout

This document is a handoff plan for growing SmallOS from the current small
passive TCP path into a real socket subsystem that can support cserve, FTP, and
future SSH at practical concurrency.

The current baseline is commit `6b820d9` (`Add cserver ELF service support`).
That commit vendors `third_party/cserver`, builds `apps/services/cserve.elf`,
adds epoll/timerfd/signalfd/user-runtime shims, raises the process fd table to
16, and gives the TCP driver several accepted streams per listen port.

Current implementation status:

- Phase 1 is implemented: process fd tables are PMM-backed, start at 16 slots,
  grow to the default 64-fd limit, and are freed on process teardown.
- Phase 2 is implemented as a first socket-object layer: socket fds point at
  `socket_t` objects that own TCP listener/connection ids.
- Phase 3 has an initial TCP scaling step: per-listener stream slots are now
  PMM-backed, capped `listen(backlog)` handling is in place, and the enlarged
  TCP table stays out of the low-memory VGA/BIOS hole. A full 4-tuple TCP
  table, per-socket wait queues, and per-connection buffer/backpressure work
  remain.

## Goals

- Support cserve, FTP, and future SSH concurrently without fd starvation.
- Make 32 to 64 simultaneous service connections boring before chasing
  hundreds.
- Keep third-party programs unchanged. Fix the OS/runtime instead.
- Replace single global TCP waiter behavior with per-object wait queues.
- Move fixed-size socket and fd state out of oversized kernel structs.
- Preserve the existing test surface while adding stress tests incrementally.

## Non-Goals For The First Pass

- Full production TCP congestion control.
- Outbound `connect()` for client programs.
- TLS or SSH protocol implementation.
- SMP safety.
- POSIX-perfect socket semantics.

Those can come later. The first milestone is a coherent server-side socket
object model with sane resource limits.

## Baseline Constraints At Start Of Rollout

### File descriptors

At the baseline, `process_t` owned a fixed `fd_entry_t fds[PROCESS_FD_MAX]`
array. `PROCESS_FD_MAX` was 16. File descriptors `0`, `1`, and `2` were stdio,
leaving 13 user-open slots.

cserve consumes baseline fds for:

- log file
- listen socket
- epoll fd
- signalfd
- timerfd
- each accepted client socket
- each static file being sent

That is why the baseline sample config used `max_conn = 4`; the current sample
now uses `max_conn = 16` after the fd-table and socket-object foundation.

### TCP

The TCP driver has a small fixed slot model:

- fixed local-port slots
- fixed accepted streams per slot
- fd carries `socket_port` and `socket_conn`
- one global TCP waiter
- minimal per-stream receive buffer

This is enough for the current smoke tests but not enough for SSH or larger
HTTP/FTP concurrency.

### Process allocation

`process_t` is expected to fit in one 4 KiB frame. Do not keep growing
`process_t` with large arrays. Move expandable state behind pointers instead.

## Phase 0: Baseline Guardrails

Purpose: make sure the next LLM starts from known-good behavior and does not
debug ghosts.

Tasks:

1. Run and record:
   - `make`
   - `make test`
   - `make socket-eof-smoke`
   - `make ftp-smoke`
   - cserve browser-shaped smoke, or an equivalent script that fetches:
     - `GET /?...large query...` with keep-alive
     - `GET /index.html`
     - `GET /favicon.ico` expecting 404
2. Confirm no QEMU process remains after tests.
3. Confirm `rg -n "dbg |tcp dbg|epoll add|epoll out|epoll wait" src tools tests`
   has no unexpected debug traces.

Exit criteria:

- All baseline tests pass.
- No temporary debug logging is committed.

## Phase 1: Dynamic Per-Process Fd Tables

Purpose: remove the fd-count ceiling from `process_t` without redesigning every
resource backend at once.

Target:

- Default per-process fd limit: 64.
- Hard cap: 256, preferably configurable by a kernel constant.
- `process_t` remains one page or gets a deliberate replacement allocation
  model.

Design:

- Replace `fd_entry_t fds[PROCESS_FD_MAX]` with:

```c
fd_entry_t* fds;
unsigned int fd_capacity;
unsigned int fd_limit;
```

- Allocate the initial fd table in `process_create()`.
- Free it in `process_destroy()`.
- Grow it when opening a new fd if `fd_capacity < fd_limit`.
- Keep fd `0`, `1`, and `2` initialized as console handles.
- Keep `PROCESS_FD_FIRST = 3`.

Important detail:

`fd_entry_t` currently contains fields used only by FAT files, sockets,
timerfd/signalfd/epoll, and console. Do not split those fields in the same
phase unless the patch stays small. It is acceptable for Phase 1 to only move
the existing array out of `process_t`.

Files likely touched:

- `src/kernel/process.h`
- `src/kernel/process.c`
- `src/kernel/syscall.c`
- `src/user/errnoprobe.c`
- `tests/elfs/errnoprobe.py`
- `docs/syscalls.md`
- `docs/architecture.md`

Tests:

- Add or update an ELF test to open at least 48 descriptors and verify
  `ENFILE` at the configured limit.
- Run `make test`, `make ftp-smoke`, and cserve smoke.

Exit criteria:

- cserve sample can safely raise `max_conn` to at least 16.
- `process_t` size no longer depends heavily on fd count.

## Phase 2: Kernel Socket Objects

Purpose: make fds references to socket objects instead of storing transport
state directly in every fd entry.

Target:

- `fd_entry_t` for sockets contains a pointer or handle to `socket_t`.
- Socket lifetime is refcounted or otherwise clearly owned.
- Socket type decides read/write/poll/close behavior.

Design sketch:

```c
typedef enum {
    SOCKET_KIND_TCP,
} socket_kind_t;

typedef enum {
    SOCKET_STATE_OPEN,
    SOCKET_STATE_BOUND,
    SOCKET_STATE_LISTENING,
    SOCKET_STATE_CONNECTED,
    SOCKET_STATE_CLOSED,
} socket_state_t;

typedef struct socket {
    socket_kind_t kind;
    socket_state_t state;
    unsigned int refs;
    unsigned int flags;
    unsigned short local_port;
    void* transport;        /* tcp_listener_t or tcp_conn_t */
    wait_queue_t read_waiters;
    wait_queue_t write_waiters;
    wait_queue_t accept_waiters;
} socket_t;
```

Do not implement every field at once if the existing kernel lacks the needed
allocator or wait queue. Add the object layer first, then migrate behavior.

Files likely touched:

- new `src/kernel/socket.h`
- new `src/kernel/socket.c`
- `src/kernel/process.c`
- `src/kernel/syscall.c`
- `src/drivers/tcp.c`
- `src/drivers/tcp.h`

Tests:

- Existing socket EOF smoke.
- FTP smoke.
- cserve keep-alive smoke.

Exit criteria:

- Closing an fd decrements/cleans the socket object.
- Accepted sockets no longer depend on fd-local `socket_port/socket_conn`
  fields.

## Phase 3: TCP Connection Table And Listen Backlog

Purpose: let TCP own many connections and let listeners queue accepted streams
properly.

Target:

- TCP connection table keyed by:
  - local IP
  - local port
  - remote IP
  - remote port
- Listen sockets own an accept queue.
- `listen(backlog)` is honored up to a kernel cap.
- `accept()` pops from the queue.

Design:

- Introduce `tcp_conn_t` objects allocated from a pool or page-backed table.
- Introduce `tcp_listener_t` for bound/listening local ports.
- Incoming SYN on a listening port allocates a `tcp_conn_t`.
- Completed handshake enqueues the connection on the listener accept queue.
- If the queue is full, drop or reset according to the simplest safe behavior.

Avoid:

- One global active port.
- One global active connection.
- Waking one global TCP waiter and hoping the caller rechecks the right object.

Files likely touched:

- `src/drivers/tcp.c`
- `src/drivers/tcp.h`
- `src/kernel/socket.c`
- `src/kernel/syscall.c`

Tests:

- Add a host-side smoke that opens N parallel TCP connections to cserve and
  holds them idle for a few seconds.
- Add a host-side smoke that rapidly opens/closes connections to FTP passive
  data port.
- Run `make socket-eof-smoke` and `make ftp-smoke`.

Exit criteria:

- cserve `max_conn = 24` works with QEMU `-m 64` or larger.
- FTP control and passive data sockets can coexist repeatedly.

## Phase 4: Wait Queues And Readiness

Purpose: remove missed wakeups and global waiter limitations.

Target:

- Generic kernel wait queue primitive.
- Sockets, timerfd, signalfd, and epoll use object-level readiness.
- `poll()` and `epoll_wait()` do not directly depend on TCP globals.

Design:

Add a small wait queue abstraction:

```c
typedef struct wait_node {
    process_t* proc;
    struct wait_node* next;
} wait_node_t;

typedef struct wait_queue {
    wait_node_t* head;
} wait_queue_t;
```

Initial implementation can be simple:

- one wait node embedded per process for now, or a small fixed wait-node pool
- `wait_queue_sleep(queue, timeout)`
- `wait_queue_wake_one(queue)`
- `wait_queue_wake_all(queue)`

Readiness rules:

- listener readable when accept queue non-empty
- connected socket readable when rx buffer non-empty or peer close visible
- connected socket writable when tx buffer has capacity and connection allows
  writing
- timerfd readable when expired
- signalfd readable when a pending synthetic signal exists

Tests:

- Repeated FTP smoke in a loop.
- Repeated cserve browser-shaped smoke in a loop.
- Add a test that blocks in `poll()` and receives data immediately after the
  poll call begins, guarding against missed wakeups.

Exit criteria:

- No global TCP waiter remains.
- `poll()`/`epoll_wait()` wake from the object that became ready.

## Phase 5: Per-Connection Buffers And Backpressure

Purpose: support more connections without losing data or assuming immediate
send completion.

Target:

- Per-connection RX ring buffer.
- Per-connection TX queue or retransmit buffer.
- Small default buffers with caps.
- `send()` can return short writes or `EAGAIN` for nonblocking sockets.

Suggested initial sizes:

- RX buffer: 4 KiB per connection.
- TX buffer/retransmit queue: 4 KiB to 16 KiB per connection.
- Global TCP memory cap.

Important:

cserve already has large user-space buffers per connection. Do not also create
huge kernel buffers by default. Hundreds of idle keep-alive sockets should be
cheap.

Tests:

- Large static file response.
- Multiple slow readers.
- Multiple clients sending partial requests.

Exit criteria:

- Static cserve responses can be served while multiple keep-alive sockets sit
  idle.
- No connection can exhaust all kernel memory without hitting a resource cap.

## Phase 6: Resource Limits And Configuration

Purpose: make scaling explicit and debuggable.

Add constants or config fields for:

- max processes
- max fds per process
- max sockets system-wide
- max TCP listeners
- max TCP connections
- max accept backlog
- default socket rx buffer size
- default socket tx buffer size
- max socket memory

Expose some visibility through shell commands:

- `netinfo` should show listeners, connection count, and maybe socket memory.
- `meminfo` should make fd/socket growth visible enough for debugging.

Suggested first practical settings:

- per-process fd limit: 128
- system sockets: 256
- TCP connections: 128
- accept backlog cap: 32
- cserve `max_conn`: 32
- cserve `keepalive_timeout_ms`: 5000
- QEMU memory: 64 MB or 128 MB for service testing

Exit criteria:

- Reasonable failures are explicit (`ENFILE`, `ENOMEM`, `EAGAIN`, 503), not
  silent resets or hung waits.

## Phase 7: Stress And Regression Tests

Purpose: keep the socket work honest as SSH arrives.

Add tools:

- `tools/cserve_smoke.py`
  - launches cserve
  - fetches large page
  - opens N keep-alive clients
  - checks no terminal log spill
- `tools/socket_parallel_smoke.py`
  - opens N connections to a simple guest service
  - sends small payloads
  - verifies all responses
- `tools/ftp_loop_smoke.py`
  - repeats PASV/LIST/RETR/STOR cycles

Make targets:

- `make cserve-smoke`
- `make socket-parallel-smoke`
- `make ftp-loop-smoke`

Run matrix:

- `make test`
- `make socket-eof-smoke`
- `make ftp-smoke`
- `make cserve-smoke`

Exit criteria:

- 32 cserve keep-alive clients pass reliably.
- FTP passive data transfers pass repeatedly.
- Socket EOF semantics stay stable.

## SSH Readiness Checklist

Before starting SSH:

- fd table is dynamic and at least 128 per process
- TCP connection table is keyed by 4-tuple
- listen backlog exists
- socket wait queues exist
- per-connection RX/TX buffers exist
- `poll()` is object-driven
- `shutdown()` has at least basic half-close semantics, or SSH integration is
  written with known limitations
- idle long-lived TCP connections do not consume large kernel buffers

SSH is interactive and long-lived. Do not start it on top of the single-waiter
or tiny fixed-fd model.

## Implementation Rules For The Next LLM

- Do not modify vendored cserver or FTP server source to hide OS gaps.
- Prefer small phases with passing tests after each phase.
- Keep old smoke tests green before adding new stress.
- Avoid increasing fixed arrays inside `process_t`.
- Keep fd/socket/TCP ownership boundaries clear:
  - `process.c`: fd lifetime and generic handle dispatch
  - `socket.c`: socket object state, wait queues, readiness
  - `tcp.c`: TCP packet state machine and connection buffers
  - `syscall.c`: validation and ABI glue
- If a syscall returns a raw TCP driver `-1`, translate it to a real errno
  before it reaches user space.
- Update docs when syscall semantics or limits change.

## Suggested First Commit For The Next LLM

Title:

`Move process fd tables out of process_t`

Scope:

- add dynamic fd table allocation
- keep current socket behavior otherwise unchanged
- update `errnoprobe` expected fd count
- update docs for fd limits
- run `make test`, `make socket-eof-smoke`, `make ftp-smoke`

Why first:

It immediately removes the cserve/FTP/SSH fd ceiling while preserving the
current TCP behavior. It is the lowest-risk foundation for the rest of the
socket subsystem.
