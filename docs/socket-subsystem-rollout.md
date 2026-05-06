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
  grow to the default 128-fd limit, and are freed on process teardown.
- Phase 2 is implemented as a first socket-object layer: socket fds point at
  `socket_t` objects that own TCP listener/connection ids.
- Phase 3 is implemented for the server-side path: passive listeners live in a
  small listener table, accepted streams live in a PMM-backed global TCP
  connection table keyed by local IP, local port, remote IP, and remote port,
  capped `listen(backlog)` handling is in place, and TCP tables stay out of the
  low-memory VGA/BIOS hole.
- Phase 4 is implemented for sockets plus timerfd/signalfd-style descriptors:
  `wait_queue_t` is backed by a fixed node pool, sockets own accept/read/write
  queues, timerfd/signalfd handles own read wait queues, and blocking
  `accept`, `recv`, socket `read`, timerfd/signalfd `read`, `poll`, and
  `epoll_wait` register on the objects they are waiting for. Timer IRQs wake
  expired timerfd waiters directly, Ctrl+C queues `SIGINT` to matching
  foreground signalfds, and shell job `kill` queues `SIGTERM` to matching job
  signalfds before falling back to force-kill behavior.
- Phase 5 has an initial receive-buffer step: accepted TCP connections allocate
  a lazy PMM-backed 4 KiB RX ring on first payload, advertise the remaining RX
  window, and stop ACKing bytes that could not be queued. It also has an
  initial send-buffer step: connected TCP streams allocate a lazy PMM-backed
  16 KiB TX ring, keep sent bytes until ACKed, release the ring once drained,
  retry buffered payloads, send zero-window probes for queued unsent data, and
  report socket writability from remaining TX capacity. FIN retransmission and
  late close cleanup have been added for the passive close path. RX/TX ring
  allocation now observes global caps.
- Phase 6 has an initial visibility/configuration step: the default process fd
  limit is 128, the sample cserve config uses `max_conn = 40` with the smoke
  gate still holding 32 keep-alive clients plus a slow reader, and `netinfo`
  reports socket-object counts, TCP listener
  and connection counts, RX/TX ring usage, allocated ring capacity, and global
  RX/TX caps.
- Phase 7 now has the first stress matrix: `make cserve-smoke` launches
  cserve, fetches the large static fixture, checks a 404, holds 32 keep-alive
  clients by default, exercises one slow reader, and captures `netinfo`;
  `make socket-parallel-smoke` drives 8 parallel tcpecho clients by default;
  `make ftp-loop-smoke` repeats FTP passive `LIST`/`RETR`/`STOR` cycles; and
  `make test` now includes an outbound `connect()` probe against a host echo
  endpoint reachable through QEMU user networking at `10.0.2.2`.

## Remaining Implementation Work

The cserve/FTP server-side milestone and the basic active-open client path are
in a useful, tested state. The remaining implementation work is mostly about
making less common TCP close paths less provisional before SSH depends on them:

- Expand TCP half-close behavior beyond the current passive FIN
  retransmission/cleanup support, especially around less common close-state
  transitions.

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
- Production-grade outbound TCP behavior beyond the default QEMU user-network
  smoke.
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
now uses `max_conn = 40` after the fd-table, socket-object, and resource
visibility foundation. The default smoke gate still holds 32 keep-alive clients
and opens one additional slow-reader connection.

### TCP

At the baseline, the TCP driver had a small fixed slot model:

- fixed local-port slots
- fixed accepted streams per slot
- fd carries `socket_port` and `socket_conn`
- one global TCP waiter
- minimal per-stream receive buffer

The current implementation has moved beyond that baseline: sockets own wait
queues, fd entries point at `socket_t` objects, listeners and global
4-tuple-keyed connection state live in PMM-backed TCP tables, and connected
streams allocate lazy 4 KiB RX rings plus 16 KiB TX rings on first use.
Remaining TCP limits are now fixed ring sizes and further close-state coverage.

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

- Initial target was a 64-fd default; the current practical default is 128.
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

Current note: the `tcp_socket_*` interface still uses a narrow active
port/connection selector as a call shim, but connection lookup and storage no
longer depend on per-listener stream arrays.

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

- TCP connection table is keyed by 4-tuple. This is now true for the passive
  server path: accepted streams use global connection ids into the PMM-backed
  table keyed by local IP, local port, remote IP, and remote port.
- cserve `max_conn = 40` works under the default QEMU memory setting while the
  smoke holds 32 keep-alive clients and one slow reader.
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
    struct wait_queue* queue;
    struct wait_node* next;
} wait_node_t;

typedef struct wait_queue {
    wait_node_t* head;
} wait_queue_t;
```

Initial implementation can be simple:

- one wait node embedded per process for now, or a small fixed wait-node pool
- `wait_queue_add(queue, proc)` and cleanup of all wait nodes for a process
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

- No global TCP waiter remains. (Implemented for socket waits.)
- `poll()`/`epoll_wait()` wake from the object that became ready. (Implemented
  for sockets, timerfd, and signalfd-backed Ctrl+C/SIGTERM delivery.)

## Phase 5: Per-Connection Buffers And Backpressure

Purpose: support more connections without losing data or assuming immediate
send completion.

Implemented so far:

- Accepted TCP connections allocate a 4 KiB PMM-backed RX ring lazily on first
  payload.
- The RX ring is released after userland drains the buffered data.
- TCP ACKs advertise the remaining RX window and do not advance the receive
  sequence past bytes that were not queued.
- `make socket-eof-smoke` now sends a 3072-byte multi-segment payload before
  the host half-close, so payload-before-EOF coverage exercises the RX ring.
- Connected TCP streams allocate a 16 KiB PMM-backed TX ring lazily on first
  write, keep queued bytes until ACKed, release the ring once drained, retry
  buffered payloads, wake socket writers when ACKs free space, make `POLLOUT`
  depend on TX capacity, and send a one-byte zero-window probe when queued
  unsent data is stuck behind a closed peer window.
- Nonblocking `send()` / socket `write()` now return short writes or `-EAGAIN`
  when the TX ring is full; blocking socket writes wait on the socket write
  queue until space returns.
- `shutdown()` has initial half-close behavior: `SHUT_RD` reports local EOF,
  `SHUT_WR` drains queued TX before sending FIN, and later writes fail with
  `EPIPE`.

Target:

- Per-connection RX ring buffer. (Initial implementation is in place.)
- Per-connection TX queue or retransmit buffer. (Initial implementation is in
  place.)
- Small default buffers with global RX/TX caps. (Initial caps are in place.)
- `send()` can return short writes or `EAGAIN` for nonblocking sockets.
  (Initial implementation is in place.)

Suggested initial sizes:

- RX buffer: 4 KiB per active receiving connection. (Current implementation.)
- TX buffer/retransmit queue: 16 KiB per connection. (Current implementation.)
- Global TCP memory cap. (Current implementation: 512 KiB RX cap and 1 MiB TX
  cap.)

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
  (Initial socket/TCP counts plus RX/TX ring usage are implemented.)
- `meminfo` should make fd/socket growth visible enough for debugging.

Suggested first practical settings:

- per-process fd limit: 128
- system sockets: 256
- TCP connections: 128
- accept backlog cap: 32
- cserve `max_conn`: 40 for the sample config; the smoke still holds 32
  keep-alive clients and one slow reader
- cserve `keepalive_timeout_ms`: 5000
- QEMU memory: 64 MB or 128 MB for service testing

Exit criteria:

- Reasonable failures are explicit (`ENFILE`, `ENOMEM`, `EAGAIN`, 503), not
  silent resets or hung waits.

## Phase 7: Stress And Regression Tests

Purpose: keep the socket work honest as SSH arrives.

Add tools:

- `tools/cserve_smoke.py` (implemented)
  - launches cserve
  - fetches large page
  - opens N keep-alive clients
  - exercises one slow reader
  - captures guest `netinfo` counters
- `tools/socket_parallel_smoke.py` (implemented)
  - opens N connections to a simple guest service
  - sends small payloads
  - verifies all responses
- `tools/ftp_loop_smoke.py` (implemented)
  - repeats PASV/LIST/RETR/STOR cycles

Make targets:

- `make cserve-smoke`
- `make socket-parallel-smoke`
- `make ftp-loop-smoke`

Run matrix:

- `make test`
- `make socket-eof-smoke`
- `make socket-parallel-smoke`
- `make ftp-smoke`
- `make ftp-loop-smoke`
- `make cserve-smoke`

Exit criteria:

- 32 cserve keep-alive clients pass reliably; `make cserve-smoke` now uses
  that as its default gate.
- FTP passive data transfers pass repeatedly.
- Socket EOF semantics stay stable.

## SSH Readiness Checklist

Before starting SSH:

- fd table is dynamic and at least 128 per process
- TCP connection table is keyed by 4-tuple
- listen backlog exists
- socket wait queues exist
- per-connection RX ring exists
- per-connection TX/retransmit buffer exists
- `poll()`/`epoll_wait()` are object-driven for sockets and timerfd
- `shutdown()` has basic half-close semantics plus passive FIN retransmission
  and late cleanup
- idle long-lived TCP connections do not consume large kernel buffers

SSH is interactive and long-lived. The old single socket waiter and tiny
fixed-fd table are gone, receive buffering has started, timerfd event-loop wakes
are object-driven, outbound `connect()` has a QEMU user-network smoke, and basic
signalfd signal production is wired. SSH should still wait for additional
shutdown-state coverage.

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

## Suggested Next Commit

Title:

`Raise cserve smoke to the 32-client gate`

Scope:

- run the full stress matrix, including `make socket-parallel-smoke`,
  `make ftp-loop-smoke`, and the default 32-client `make cserve-smoke`
- keep 32 as the default `CSERVE_SMOKE_CLIENTS` gate
- keep `netinfo` captures in the stress logs so socket/TCP counts and RX/TX
  ring usage remain visible

Why first:

The broader Phase 7 stress tools now exist. The next useful proof point is
turning the original cserve exit criterion from an opt-in target into the
default gate before moving back into larger TCP internals.
