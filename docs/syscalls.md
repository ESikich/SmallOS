# Syscall Interface (int 0x80)

This document defines the syscall ABI between ELF programs and the kernel.

⚠️ This is a **strict contract**. Any change here must be reflected in both:

* `interrupts.asm` (`isr128_stub`)
* `syscall_regs_t` in `syscall.h`

---

## Invocation

```asm
int 0x80
```

---

## Register Convention

```text
eax = syscall number
ebx = arg1
ecx = arg2
edx = arg3
esi = arg4 for four-argument calls

return value → eax
```

---

## Error Convention

Syscalls return non-negative results on success and negative errno values on
failure. Low-level `sys_*` helpers expose those raw values directly; POSIX-style
user runtime wrappers return `-1` and set `errno` to the positive error code.

Shared errno values:

| Name | Value | Meaning |
| --- | ---: | --- |
| `EPERM` | 1 | operation not permitted |
| `ENOENT` | 2 | no such file or directory |
| `ESRCH` | 3 | no such process |
| `EINTR` | 4 | interrupted system call |
| `EIO` | 5 | input/output error |
| `ENXIO` | 6 | no such device or address |
| `EBADF` | 9 | bad file descriptor |
| `ECHILD` | 10 | no child processes |
| `EAGAIN` / `EWOULDBLOCK` | 11 | resource temporarily unavailable |
| `ENOMEM` | 12 | out of memory |
| `EACCES` | 13 | permission denied |
| `EFAULT` | 14 | bad user address |
| `EBUSY` | 16 | resource busy |
| `EEXIST` | 17 | file exists |
| `ENODEV` | 19 | no such device |
| `ENOTDIR` | 20 | not a directory |
| `EISDIR` | 21 | is a directory |
| `EINVAL` | 22 | invalid argument |
| `ENFILE` | 23 | descriptor table full |
| `EFBIG` | 27 | file too large |
| `EPIPE` | 32 | broken pipe |
| `ENAMETOOLONG` | 36 | path or name too long |
| `ENOSYS` | 38 | function not implemented |
| `ENOTEMPTY` | 39 | directory not empty |
| `EPROTO` | 71 | protocol error |
| `EOVERFLOW` | 75 | value too large |
| `EMSGSIZE` | 90 | message too long |
| `EPROTONOSUPPORT` | 93 | protocol not supported |
| `EOPNOTSUPP` | 95 | operation not supported |
| `EADDRINUSE` | 98 | address already in use |
| `ENETUNREACH` | 101 | network unreachable |
| `ECONNRESET` | 104 | connection reset |
| `EISCONN` | 106 | socket is already connected |
| `ETIMEDOUT` | 110 | connection timed out |
| `ECONNREFUSED` | 111 | connection refused |
| `EHOSTUNREACH` | 113 | host unreachable |
| `EALREADY` | 114 | operation already in progress |
| `EINPROGRESS` | 115 | operation now in progress |

---

## User Pointer Validation

Syscalls that read from or write to user memory validate the full byte range
before dereferencing it. A pointer must live in the user address window, every
touched page must be present and user-accessible in the caller's page tables,
and variable-length arrays must pass byte-count overflow checks.

Invalid user pointers return `-EFAULT`. Oversized counts that would wrap their
byte length, such as huge `poll()` / `epoll_wait()` arrays, return `-EINVAL`.
The `usr/libexec/tests/badptrprobe` regression covers unmapped pointers, buffers and
output structs that cross from a mapped page into an unmapped page, and wrapped
array counts.

---

## Current Syscalls

### SYS_WRITE (1)

```c
int sys_write(const char* buf, uint32_t len);
```

Writes `len` bytes from `buf` to fd `1` for the calling process, falling back
to the kernel terminal only when no current stdout handle is available. Returns
bytes written or a negative errno value.
Terminal control characters are interpreted by the shared terminal path:
`\n` advances to the next line, `\r` returns to column 0, and `\b` erases the
previous VGA cell when possible.
The kernel routes this through the bulk terminal path so framebuffer backends
can bracket expensive redraw work around the whole write. User programs that
produce large output should batch writes up to the syscall limit rather than
issuing one write per rendered row.
The terminal does not implement automatic page prompts; long-output paging is
provided by userland commands such as `more` so `write()` never unexpectedly
blocks on terminal input.
This remains available as the low-level write primitive used by `u_puts()` and
early/simple user helpers. The normal POSIX/stdio path writes `stdout` and
`stderr` through fd-backed console or PTY handles via `SYS_WRITEFD`.

---

### SYS_EXIT (2)

```c
void sys_exit(int code);
```

Terminates the current ring-3 process. Does not return to the caller. `sys_exit_impl()` switches to the kernel page directory and calls `sched_exit_current((unsigned int)regs)`. The task becomes `PROCESS_STATE_ZOMBIE`, is dequeued, and execution switches to the next runnable task. Destruction is deferred to a later waiter such as `process_wait()`.

---

### SYS_GET_TICKS (3)

```c
uint32_t sys_get_ticks(void);
```

Returns PIT tick count since boot.

---

### SYS_PUTC (4)

```c
int sys_putc(char c);
```

Writes a single character to fd `1` for the calling process, with the same
terminal fallback as `SYS_WRITE`. Returns 1 on success.

---

### SYS_READ (5)

```c
int sys_read(char* buf, uint32_t len);
```

Blocks until keyboard input is available, echoing each character. Terminates early on newline (included in returned data). Returns bytes read or a negative errno value.

`SYS_READ` is implemented as a read from fd `0`, which is initialized as a
console handle in every user process and may later be inherited as a PTY slave
by GUI-launched shells and their children. The console handle uses true
scheduler-aware blocking: when `kb_buf` is empty it sets the process state to
`PROCESS_STATE_WAITING` and registers it as the keyboard waiter via
`keyboard_set_waiting_process()`, then executes `hlt`. The timer IRQ fires
normally; `sched_tick` sees the task is `WAITING`, skips it, and switches to
another runnable task. When a keypress arrives, `process_key_consumer()` pushes
the character into `kb_buf`, sets the waiting process back to
`PROCESS_STATE_RUNNING`, and clears the waiter slot. On the next scheduler pass
the process is selected, resumes after the `hlt`, and drains the buffer
normally.

`sti` is issued before the first `hlt` so IRQ1 can fire during the wait. `cli` is restored before returning, matching the IF=0 postcondition expected by the syscall gate.

---

### SYS_YIELD (6)

```c
void sys_yield(void);
```

Voluntarily gives the machine a chance to service interrupts. The current
implementation enables interrupts, halts until the next interrupt, disables
interrupts again, and returns `0` to the caller. Timer IRQs still own actual
round-robin switching, so `SYS_YIELD` is cooperative wait-for-interrupt rather
than an immediate direct call into `sched_yield_now()`.

---

### SYS_EXEC (7)

```c
int sys_exec(const char* name, int argc, char** argv);
```

Legacy spawn-style process creation. Loads and asynchronously spawns a named
ELF program through the kernel VFS layer. Returns the child pid on success or a
negative errno if validation, lookup, or load fails. The child is claimed for
the caller until `SYS_WAITPID` collects it or the parent exits. New
POSIX-shaped code should prefer `SYS_FORK` plus `SYS_EXECVE`.

`sys_exec_impl` copies `name` to a local kernel stack buffer before any VFS or ELF work so the loader does not depend on the caller's user pointer remaining valid. It then calls `elf_run_named()`, which loads the program through a private PMM-backed image buffer, creates the process, seeds its scheduler bootstrap context, enqueues it, frees the temporary image buffer, and returns immediately.

---

### SYS_OPEN (8)

```c
int sys_open(const char* name);
```

Legacy shorthand for `SYS_OPEN_MODE_READ`. Opens an ext2 file by path
(case-sensitive native ext2 matching per component). Allocates the lowest free
slot in the calling process's handle table (fd >= 3). The fd points to a shared
open-file description containing the current offset, status flags, cached file
data, and backing filename. Duplicated descriptors and fork-inherited
descriptors share that description. fds 0/1/2 are pre-opened console handles.

Returns the fd (≥ 3) on success, or a negative errno if the file is not found,
the path names a directory, the process handle table is full, or the name
pointer fails user-space validation. The fd table starts with 16 slots, grows on
demand, and defaults to a 128-fd per-process limit.

`sys_open_impl` validates the name with page-aware user checks, copies it into a kernel buffer bounded by `PROCESS_FD_NAME_MAX` (128 bytes), then calls through the VFS stat wrapper to confirm the file exists without loading its data.

---

### SYS_CLOSE (9)

```c
int sys_close(int fd);
```

Closes an open handle, freeing its slot for reuse. Returns `0` on success or a
negative errno if the fd is out of range, not currently open, or no current
process exists.

---

### SYS_FREAD (10)

```c
int sys_fread(int fd, char* buf, uint32_t len);
```

Reads up to `len` bytes from an open readable handle at `fd` into `buf`. File handles read from the current file position and advance it by the number of bytes actually read. Socket handles read from the TCP receive path, which uses the connected stream's lazy 4 KiB PMM-backed RX ring. Socket writes use a lazy 16 KiB PMM-backed TX ring, observe global RX/TX caps, and report writable readiness from remaining TX capacity. fd `0` reads from the console input buffer with the same blocking behavior as `SYS_READ`. Returns the number of bytes read, `0` at end-of-file / closed socket, or a negative errno on error.

For file handles, the read op loads the file once into PMM-backed shared file
cache pages on first use, then copies the requested slice from that cache into
the validated user buffer. The cache stays live until the shared open-file
description is closed, so duplicated and fork-inherited descriptors reuse it
and share the current file offset.

---

### SYS_SLEEP (11)

```c
int sys_sleep(uint32_t ticks);
```

Blocks the calling process for at least `ticks` timer ticks. The task marks
itself `PROCESS_STATE_SLEEPING`, parks with interrupts enabled, and is woken by
the timer IRQ once the deadline is reached.

`sys_sleep_impl` marks the task sleeping until `timer_get_ticks()` reaches the
stored wake deadline. If no other runnable task exists, the kernel still idles
in a `hlt` loop until the wake condition is met.

---

### SYS_WRITEFILE (12)

```c
int sys_writefile(const char* name, const char* buf, uint32_t len);
```

Creates or overwrites a root-directory ext2 file in one shot. Returns `0` on success or a negative errno on failure, including when the active ext2 source is read-only USB storage.

This is the root-only persistence primitive for generated artifacts such as compiler output, assembly listings, or other build products. The kernel validates both the filename and the byte range before calling into the VFS root-write wrapper.

---

### SYS_HALT (13)

```c
int sys_halt(void);
```

Halts the machine through the kernel system path. Used by the `/bin/halt`
command ELF and the halt smoke test.

---

### SYS_REBOOT (14)

```c
int sys_reboot(void);
```

Requests a machine reboot through the kernel system path. Used by the
`/bin/reboot` command ELF and the reboot smoke test.

---

### SYS_WRITEFILE_PATH (15)

```c
int sys_writefile_path(const char* path, const char* buf, uint32_t len);
```

Creates or overwrites an ext2 file at an arbitrary path. Returns `0` on success or a negative errno on failure, including when the active ext2 source is read-only USB storage.

This is the preferred persistence primitive for build tools and compilers because it can emit directly into nested writable directories such as `/var/tmp/WORK/` or `/var/tmp/generated/`. The kernel validates both the path and the byte range before calling into the VFS path-write wrapper.

---

### SYS_BRK (16)

```c
uint32_t sys_brk(uint32_t new_brk);
```

Queries or adjusts the calling process heap break. Passing `0` returns the current break. The kernel may grow or shrink the heap on page boundaries.

The user-space allocator in `src/user/libc/malloc.c` is layered on top of this syscall.

---

### SYS_OPEN_WRITE (17)

```c
int sys_open_write(const char* name);
```

Legacy shorthand for `SYS_OPEN_MODE_WRITE | SYS_OPEN_MODE_CREATE |
SYS_OPEN_MODE_TRUNC`. Opens an ext2 file for streaming write/truncate and
returns a writable file-backed handle. New runtime code should prefer
`SYS_OPEN_MODE` so read/write/append/truncate intent is explicit. This fails
through the normal negative-errno path when ext2 is mounted from read-only USB
storage.

---

### SYS_WRITEFD (18)

```c
int sys_writefd(int fd, const char* buf, uint32_t len);
```

Writes bytes to an open writable handle. Returns bytes written or a negative
errno-style value on error. ext2 file descriptors stream the bytes to the
current file offset, allocate blocks as the file grows, preserve untouched
bytes in partial sectors, and zero-fill gaps created by seek-past-EOF writes.
Descriptors `1` and `2` are fd-backed console handles, so `write(1, ...)`,
`write(2, ...)`, `printf`, and `fprintf(stderr, ...)` all travel through this
same handle path. User-opened files and sockets still start at fd `3`.

---

### SYS_LSEEK (19)

```c
int sys_lseek(int fd, int offset, int whence);
```

Repositions a seekable handle. ext2 file handles support this today; console
and socket handles return `-ENOSYS`.

---

### SYS_UNLINK (20)

```c
int sys_unlink(const char* path);
```

Removes an ext2 file.

---

### SYS_RENAME (21)

```c
int sys_rename(const char* src, const char* dst);
```

Renames or moves an ext2 entry.

---

### SYS_STAT (22)

```c
int sys_stat(const char* path, uint32_t* out_size, int* out_is_dir);
```

Queries whether an ext2 path exists and whether it resolves to a file or directory. For regular files, `out_size` receives the file size. For directories, `out_is_dir` is set to 1 and `out_size` is set to 0.

---

### SYS_SOCKET (23)

```c
int sys_socket(int domain, int type, int protocol);
```

Creates a socket handle. The TCP stream path accepts `AF_INET`,
`SOCK_STREAM`, and either `0` or `IPPROTO_TCP`. The networking compatibility
path also accepts `AF_INET`/`SOCK_DGRAM` as a control/datagram descriptor for
network ioctls, DNS lookups, BusyBox `tftp`, and BusyBox `udpsvd`/`tftpd`;
BusyBox `udhcpc` uses `SYS_NET_OP_DHCP` instead of this raw socket path.
`AF_INET`/`SOCK_RAW`/`IPPROTO_ICMP` is accepted for raw ICMP echo traffic used
by BusyBox `ping`. `AF_NETLINK` with `NETLINK_ROUTE` creates a minimal
rtnetlink descriptor for BusyBox `ip link`, `ip addr`, `ip route`, and
`ip neigh` over the current `eth0`, loopback, route, and ARP-neighbor state.

---

### SYS_BIND (24)

```c
int sys_bind(int fd, const struct sockaddr* addr, socklen_t addrlen);
```

Associates a socket with an IPv4 address/port tuple. For `AF_NETLINK`
descriptors, `bind` accepts `struct sockaddr_nl`; pid `0` is replaced with the
current process pid and multicast group bits are stored for compatibility.

`sin_port` is interpreted in network byte order, matching the user-space wrappers.

---

### SYS_LISTEN (25)

```c
int sys_listen(int fd, int backlog);
```

Puts a bound socket into passive-listen mode. `backlog` is honored up to the
kernel socket backlog cap; values below 1 become a single pending connection.

---

### SYS_ACCEPT (26)

```c
int sys_accept(int fd, struct sockaddr* addr, socklen_t* addrlen);
```

Waits for an incoming TCP connection on a listening socket and returns a
connected handle. Blocking accepts park on the listening socket's accept wait
queue; nonblocking sockets or `accept4(..., SOCK_NONBLOCK)` return `-EAGAIN`
when no accepted connection is queued.

If `addr` and `addrlen` are supplied, the kernel writes back the peer address
using network byte order for the address and port fields.

---

### SYS_CONNECT (27)

```c
int sys_connect(int fd, const struct sockaddr* addr, socklen_t addrlen);
```

Starts an outbound TCP active open for an IPv4 stream socket. Open or bound
sockets may connect; unbound sockets receive an ephemeral local port. Blocking
connect waits for the SYN/SYN-ACK/ACK handshake to complete or for the TCP
retry path to fail. Nonblocking sockets return `-EINPROGRESS` while the
handshake is pending; `poll(..., POLLOUT)` reports completion, while
`POLLERR`/`POLLHUP` report connection failure.

---

### SYS_SEND (28)

```c
int sys_send(int fd, const void* buf, uint32_t len);
```

Sends bytes on an established stream socket. TCP streams are stored in the
global 4-tuple TCP table, queue bytes in a lazy 16 KiB PMM-backed
per-connection TX ring, transmit from that ring, keep sent bytes until ACKed,
release the ring once drained, retry buffered payloads, and send zero-window
probes for queued unsent data. Blocking socket writes wait on the socket write
wait queue when the TX ring is full. Nonblocking socket writes can return a
short byte count or `-EAGAIN`. After `shutdown(fd, SHUT_WR)`, sends fail with
`-EPIPE`.

---

### SYS_RECV (29)

```c
int sys_recv(int fd, void* buf, uint32_t len);
```

Receives bytes from an established stream socket. TCP sockets block on their
read wait queue until data arrives or the connection closes, or return EOF
immediately after local `SHUT_RD`. Connected TCP streams are looked up from the
global 4-tuple TCP table, use a lazy PMM-backed 4 KiB receive ring, and
advertise the remaining receive window to the peer.

---

### SYS_SENDTO (143)

```c
int sys_sendto(int fd,
               const void* buf,
               uint32_t len,
               unsigned int flags,
               const struct sockaddr* dest,
               socklen_t destlen);
```

Sends socket payloads with an explicit destination. TCP stream descriptors
without a destination are forwarded to `SYS_SEND`; raw ICMP descriptors wrap
the supplied ICMP bytes in an IPv4 packet and route them through the existing
IPv4/ARP next-hop path. UDP descriptors build IPv4/UDP datagrams, bind an
ephemeral source port when needed, and use the same route/ARP path. UDP
checksums are currently omitted; the covered guest surface is DNS-sized
resolver traffic plus the current BusyBox TFTP client and `udpsvd`/`tftpd`
service smoke paths. Netlink route descriptors accept
`RTM_GETLINK`, `RTM_GETADDR`, `RTM_GETROUTE`, `RTM_NEWADDR`, `RTM_DELADDR`,
`RTM_NEWROUTE`, and `RTM_DELROUTE` request messages for the single `eth0`
interface.

---

### SYS_RECVFROM (144)

```c
int sys_recvfrom(int fd,
                 void* buf,
                 uint32_t len,
                 unsigned int flags,
                 struct sockaddr* src,
                 socklen_t* srclen);
```

Receives socket payloads and optionally writes the peer IPv4 source address.
TCP stream descriptors without a source buffer are forwarded to `SYS_RECV`.
Raw ICMP descriptors block until an ICMP IPv4 packet is delivered by the NIC
receive path, then return the IPv4 header plus ICMP payload, matching the shape
expected by BusyBox `ping`. UDP descriptors receive queued IPv4/UDP datagrams
for their bound local port; connected UDP sockets match their peer before a
wildcard bound socket on the same local port, which lets BusyBox
`udpsvd`/`tftpd` receive TFTP acknowledgements without the parent service
stealing them. The first queue is intentionally small and sized for DNS/TFTP
smoke traffic rather than high-throughput UDP applications. UDP receives honor
`MSG_PEEK` for the service discovery pattern used by BusyBox `udpsvd`. Netlink
route descriptors return queued rtnetlink response messages and honor
`MSG_PEEK` and `MSG_DONTWAIT`.

---

### SYS_POLL (30)

```c
int sys_poll(struct pollfd* fds, nfds_t nfds, int timeout);
```

Checks readiness by asking each handle's `poll` operation. Current handle
support includes socket readiness, writable console descriptors, readable
console input when a key is already buffered, and basic file readability /
writability. `timeout` follows the POSIX millisecond convention and is rounded
up to the configured timer tick rate for sleeping waits. Socket polls register
with socket-owned accept/read/write queues while sleeping.

---

### SYS_MKDIR (31)

```c
int sys_mkdir(const char* path, uint32_t mode);
```

Creates an ext2 directory at `path`. The kernel applies the caller's `umask`,
stores the effective uid/gid on the new inode, and requires write+execute
permission on the parent directory unless the caller is root.

---

### SYS_RMDIR (32)

```c
int sys_rmdir(const char* path);
```

Removes an existing empty ext2 directory. Removing `/` is rejected by the
filesystem driver.

---

### SYS_DIRLIST (33)

```c
int sys_dirlist(const char* path, uint32_t index, struct uapi_dirent* out);
```

Copies the zero-based directory entry at `index` into `out` and returns `1`.
Returns `0` when the index is past the end of the directory. The returned
record contains `d_name`, `d_size`, and `d_is_dir`.
User-space should prefer the batched directory syscall below through
`readdir()`; `SYS_DIRLIST` remains as the simple indexed ABI.

---

### SYS_SETSOCKOPT (34)

```c
int sys_setsockopt(int fd, int level, int optname,
                   const void* optval, socklen_t optlen);
```

Validates that `fd` is a socket and currently treats most options as
compatibility no-ops. `SO_REUSEADDR` is recognized and enables UDP bind sharing
when every socket on the local port has requested it; `SO_BROADCAST` returns
success for BusyBox compatibility. The raw kernel syscall consumes `fd`,
`level`, and `optname`; the user helper has the POSIX-shaped `optval` /
`optlen` arguments and ignores them. This keeps common server code portable
while the kernel socket option surface stays small.

---

### SYS_GETSOCKNAME (35)

```c
int sys_getsockname(int fd, struct sockaddr* addr, socklen_t* addrlen);
```

Writes back the local IPv4 socket address for a socket handle using network
byte order for the address and port fields.

---

### SYS_OPEN_MODE (36)

```c
int sys_open_mode(const char* name, uint32_t mode);
```

Mode-aware ext2 open. `mode` is a bitmask of:

```c
SYS_OPEN_MODE_READ
SYS_OPEN_MODE_WRITE
SYS_OPEN_MODE_CREATE
SYS_OPEN_MODE_TRUNC
SYS_OPEN_MODE_APPEND
```

This is the preferred descriptor-open primitive for POSIX `open()` and stdio
`fopen()` because it can represent read-only, write-truncate, read/write,
create, and append opens without forcing every write-capable path to truncate.
Writable file handles stream writes through ext2 at the descriptor offset.
Partial-sector writes preserve surrounding bytes, append starts at the current
file size, and seek-past-EOF writes zero-fill the gap. Write-capable opens fail
when the current ext2 mount is read-only USB storage.

---

### SYS_GETCWD (37)

```c
int sys_getcwd(char* buf, uint32_t size);
```

Copies the calling process's current working directory into `buf` as an
absolute display path such as `/` or `/usr/bin`. Returns `0` on success or
a negative errno if the user buffer is invalid or too small.

---

### SYS_CHDIR (38)

```c
int sys_chdir(const char* path);
```

Changes the calling process's current working directory. Relative paths are
resolved against the process cwd, `.` and `..` are normalized, and the result
must name an existing ext2 directory. File-oriented syscalls such as
`SYS_OPEN`, `SYS_OPEN_MODE`, `SYS_STAT`, `SYS_RENAME`, `SYS_UNLINK`,
`SYS_MKDIR`, `SYS_RMDIR`, `SYS_DIRLIST`, `SYS_WRITEFILE_PATH`, and `SYS_EXEC`
resolve relative paths through the same process cwd.

---

### SYS_FSYNC (39)

```c
int sys_fsync(int fd);
```

Flushes a writable descriptor. Current streaming ext2 writes are committed as
the write calls run, so this is mostly a stdio contract hook: writable file
streams call it from `fflush()`, and non-writable handles return success.

---

### SYS_READ_RAW (40)

```c
int sys_read_raw(char* buf, uint32_t len);
```

Reads console input like `SYS_READ`, but does not echo bytes through the
terminal. Full-screen programs use this to consume ordinary characters plus
ANSI-style special-key sequences for arrows, Home/End, Delete, PageUp/PageDown,
and function keys.

---

### SYS_FCNTL (41)

```c
int sys_fcntl(int fd, int cmd, uint32_t arg);
```

Supports descriptor flags through `SYS_FCNTL_GETFD` / `F_GETFD` and
`SYS_FCNTL_SETFD` / `F_SETFD`, plus status flags through `SYS_FCNTL_GETFL` /
`F_GETFL` and `SYS_FCNTL_SETFL` / `F_SETFL`. `FD_CLOEXEC` is per descriptor;
`O_NONBLOCK` is stored on the shared open-file description so duplicated and
fork-inherited descriptors observe the same nonblocking state. Unsupported
commands return `-EINVAL`.

`SYS_FCNTL_DUPFD` / `F_DUPFD` and `SYS_FCNTL_DUPFD_CLOEXEC` /
`F_DUPFD_CLOEXEC` duplicate a descriptor at or above the requested minimum fd.
They share the same underlying open-file description as `dup`/`dup2`; the
`CLOEXEC` variant sets `FD_CLOEXEC` only on the new descriptor.

---

### SYS_EPOLL_CREATE (42)

```c
int sys_epoll_create(int flags);
```

Creates an epoll handle. `EPOLL_CLOEXEC` sets `FD_CLOEXEC` on the returned
descriptor.

---

### SYS_EPOLL_CTL (43)

```c
int sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event* event);
```

Adds, modifies, or deletes a watched descriptor from an epoll handle. Watches
are stored in a PMM-backed page and are currently capped at 64 entries.
`EPOLL_CTL_ADD`, `EPOLL_CTL_MOD`, and `EPOLL_CTL_DEL` are supported.

---

### SYS_EPOLL_WAIT (44)

```c
int sys_epoll_wait(int epfd, struct epoll_event* events,
                   int maxevents, int timeout);
```

Scans watched descriptors through their handle `poll` operations and sleeps
until readiness or timeout. Socket watches register with socket-owned
accept/read/write wait queues while sleeping, and timerfd/signalfd-style
handles register with per-handle read wait queues. Expired timerfds are woken
from the timer IRQ path instead of by epoll-specific deadline scanning.

---

### SYS_TIMERFD_CREATE (45)

```c
int sys_timerfd_create(int clock_id, int flags);
```

Creates a timerfd handle for `CLOCK_REALTIME` or `CLOCK_MONOTONIC`.
`TFD_NONBLOCK` is persisted as descriptor nonblocking state; `TFD_CLOEXEC`
sets `FD_CLOEXEC` on the returned descriptor.

---

### SYS_TIMERFD_SETTIME (46)

```c
int sys_timerfd_settime(int fd, int flags,
                        const struct itimerspec* new_value,
                        struct itimerspec* old_value);
```

Arms or disarms a timerfd using seconds/nanoseconds converted to kernel timer
ticks. One-shot and periodic timers are supported. `flags` must be `0`; when
`old_value` is supplied the current implementation writes back zeros.

---

### SYS_SIGNALFD (47)

```c
int sys_signalfd(int fd, const sigset_t* mask, int flags);
```

Creates or reconfigures a signalfd handle. `SFD_NONBLOCK` is persisted as
descriptor nonblocking state and `SFD_CLOEXEC` sets `FD_CLOEXEC`.
Kernel terminal interrupts now queue `SIGINT` to matching signalfds in the
foreground process group before falling back to Ctrl+C group termination, and
shell job `kill` queues `SIGTERM` to matching job-group signalfds before
force-killing the group.
Reads return one `struct signalfd_siginfo` for a pending masked signal, or
`-EAGAIN` for nonblocking reads when none is queued.

---

### SYS_ACCEPT4 (48)

```c
int sys_accept4(int fd, struct sockaddr* addr,
                socklen_t* addrlen, int flags);
```

Accepts a queued TCP connection like `SYS_ACCEPT`, with support for
`SOCK_NONBLOCK` and `SOCK_CLOEXEC` on the returned descriptor.

---

### SYS_SHUTDOWN (49)

```c
int sys_shutdown(int fd, int how);
```

Applies TCP half-close behavior to a connected socket. `SHUT_RD` makes future
reads return EOF and discards queued/incoming payload bytes, `SHUT_WR` sends a
FIN after any queued TX bytes drain and makes future writes fail with `EPIPE`,
and `SHUT_RDWR` applies both directions.

---

### SYS_GETPEERNAME (50)

```c
int sys_getpeername(int fd, struct sockaddr* addr, socklen_t* addrlen);
```

Writes back the IPv4 peer address for a connected socket using network byte
order for the address and port fields.

---

### SYS_FSTAT (51)

```c
int sys_fstat(int fd, uint32_t* out_size, int* out_is_dir);
```

Reports descriptor metadata from the open handle. File descriptors report the
cached file size and directory flag; non-file handles report their current
handle metadata.

### SYS_TERMINAL_SIZE (52)

```c
int sys_terminal_size(uint32_t* out_rows, uint32_t* out_cols);
```

Writes the active terminal backend dimensions. Full-screen user programs such
as `edit` use this instead of assuming a legacy fixed-size text layout. If the
calling process reads from a PTY slave, the PTY's current rows and columns are
reported.

---

### SYS_DISPLAY_INFO (53)

```c
int sys_display_info(sys_display_info_t* out_info);
```

Writes framebuffer geometry and format. The current graphics path expects
`SYS_DISPLAY_FORMAT_XRGB8888` with `bpp == 32`; if the system is booted with
`DISPLAY_BACKEND=vga`, framebuffer graphics are reported as unavailable.

---

### SYS_DISPLAY_FILL (54)

```c
int sys_display_fill(uint32_t x, uint32_t y, uint32_t w,
                     uint32_t h, uint32_t color);
```

Fills a rectangle with an XRGB8888 color. The caller must hold the display via
`SYS_DISPLAY_ACQUIRE`.

---

### SYS_DISPLAY_BLIT (55)

```c
int sys_display_blit(uint32_t x, uint32_t y, uint32_t w,
                     uint32_t h, const uint32_t* pixels);
```

Copies XRGB8888 pixels from user memory into the framebuffer. User graphics
programs normally draw into `src/user/gfx.c`'s backbuffer and present the whole
screen with one blit. When the framebuffer backend has multiple pages, this
copy-style syscall updates each page instead of flipping scanout, preserving
normal dirty-rectangle semantics for apps such as `gui`.

---

### SYS_DISPLAY_ACQUIRE (56)

```c
int sys_display_acquire(void);
```

Enters exclusive graphics drawing mode for the calling process. Returns a
negative errno if no framebuffer is available or another process owns the
display.

---

### SYS_DISPLAY_RELEASE (57)

```c
int sys_display_release(void);
```

Leaves graphics drawing mode. The kernel also releases display ownership when
the owning process exits.

---

### SYS_MOUSE_READ (58)

```c
int sys_mouse_read(sys_mouse_state_t* out_state);
```

Copies the current mouse state into user memory and clears the accumulated
relative movement counters. The driver may source those deltas from standard
PS/2 packets, from VMware absolute-pointer events converted to relative motion,
or from an OHCI USB boot mouse injecting relative reports into the same mouse
state. `dx` and `dy` are signed screen-space deltas since the previous
successful read, with positive `dy` moving downward. `wheel` is the accumulated
vertical wheel delta. `buttons` uses
`SYS_MOUSE_BUTTON_LEFT`, `SYS_MOUSE_BUTTON_RIGHT`, and
`SYS_MOUSE_BUTTON_MIDDLE`; `sequence` increments when a mouse event is decoded.

Returns `0` on success, `-EFAULT` for an invalid output pointer, or `-EIO` when
no PS/2, VMware, or USB mouse source has made the mouse driver available. This
small polling primitive is kept for existing graphics demos.

---

### SYS_INPUT_READ (59)

```c
int sys_input_read(sys_input_event_t* out_events,
                   unsigned int max_events,
                   unsigned int flags);
```

Reads queued keyboard and mouse events into user memory and returns the number
of events copied. Key events report key code, ASCII value when one exists,
pressed/released state, modifier flags, tick count, and input sequence number.
Mouse events report screen-space `dx`/`dy`, vertical `wheel` delta, current
buttons, changed buttons, tick count, and input sequence number.

With `flags == 0`, the syscall blocks until at least one event is available.
With `SYS_INPUT_FLAG_NONBLOCK`, it returns `0` immediately when the queue is
empty. `max_events` must be between 1 and 64.

Returns a positive event count, `0` for an empty nonblocking read or zero
length request, `-EFAULT` for an invalid output buffer, or `-EINVAL` for
unsupported flags or an oversized request.

---

### SYS_FSINFO (60)

```c
int sys_fsinfo(sys_fsinfo_t* out_info);
```

Writes ext2 volume usage information into user memory. The reported byte
counts cover allocatable data blocks: total, used, free, block size, total
blocks, and free blocks.

Returns `0` on success, `-EFAULT` for an invalid output pointer, or `-EIO` if
the ext2 usage scan fails.

---

### SYS_FSMAP (61)

```c
int sys_fsmap(sys_fsmap_request_t* req);
```

Copies ext2 allocation states for the data area into `req->states`. The
request starts at zero-based ext2 data-block index `start_cluster` and copies
up to `max_clusters` bytes. The field names keep the original ABI spelling,
but the units are ext2 allocation blocks. Each returned byte is `0` for free
or `1` for used; `out_clusters` receives the count actually written. Block
index 0 maps to ext2 block 20.

Returns `0` on success, `-EFAULT` for invalid request or output buffers, or
`-EIO` if the ext2 bitmap scan fails.

---

### SYS_GETPID (62)

```c
int sys_getpid(void);
```

Returns the current process id.

---

### SYS_WAITPID (63)

```c
int sys_waitpid(int pid, int* status, int options);
```

Waits for a direct child created by legacy `SYS_EXEC` or POSIX-shaped
`SYS_FORK`. `pid > 0` waits for that child; `pid == -1` waits for any child.
`SYS_WAITPID_WNOHANG` returns `0` immediately when the selected child has not
exited. `SYS_WAITPID_WUNTRACED` also reports a selected stopped child once
without reaping it, using the POSIX stopped-status encoding. On exit success,
returns the collected child pid and writes a POSIX-style wait status when
`status` is non-null. Missing children return `-ECHILD`.

---

### SYS_KILL (64)

```c
int sys_kill(int pid, int signum);
```

Delivers a SmallOS terminal/process signal. Positive `pid` targets one process,
`pid == 0` targets the caller's process group, and negative `pid` targets that
process group id. Signal `0` performs existence checks. Stop signals mark user
processes stopped, `SIGCONT` resumes stopped processes, and other terminating
signals exit targets with status `128 + signum`. Invalid pids or groups return
`-ESRCH`; invalid signal numbers return `-EINVAL`.

---

### SYS_DIRLIST_BATCH (65)

```c
int sys_dirlist_batch(const char* path,
                      uint32_t start_index,
                      struct uapi_dirent* out,
                      uint32_t max_count);
```

Copies up to `max_count` directory entries beginning at `start_index` and
returns the number copied. The kernel caps each call at 64 entries. Userland
`readdir()` uses this syscall as a small cache so tools like `ls` and `tree`
avoid rescanning a directory from zero for every entry.

---

### SYS_CLOCK_GETTIME (66)

```c
int sys_clock_gettime(int clock_id, struct timespec* ts);
```

Writes a seconds/nanoseconds timestamp into `ts`. `CLOCK_MONOTONIC` reports
uptime, while `CLOCK_REALTIME` reports the settable wall clock maintained as
an offset from uptime.

Returns `0` on success, `-EINVAL` for an unsupported clock id, or `-EFAULT`
for an invalid output pointer.

---

### SYS_CLOCK_SETTIME (67)

```c
int sys_clock_settime(int clock_id, const struct timespec* ts);
```

Sets `CLOCK_REALTIME`. `CLOCK_MONOTONIC` is read-only. Nanoseconds must be in
the normal `[0, 1000000000)` range; the current implementation stores seconds
precision.

Returns `0` on success, `-EINVAL` for an unsupported clock id or invalid
nanoseconds, or `-EFAULT` for an invalid input pointer.

---

### SYS_NTP_SYNC (68)

```c
int sys_ntp_sync(uint32_t server_ip, struct timespec* out_ts);
```

Queries an IPv4 NTP server through the kernel's tiny UDP/NTP path, sets
`CLOCK_REALTIME` from the response transmit timestamp, and optionally writes
the synchronized time to `out_ts`. `server_ip` is in the same host-order IPv4
form used internally by the network drivers, for example `0x81060F1C` for
`129.6.15.28`.

Returns `0` on success, `-ETIMEDOUT` when ARP/send/reply waiting fails, or
`-EFAULT` for an invalid output pointer.

---

### SYS_PIPE (69)

```c
int sys_pipe(int fds[2]);
```

Creates a unidirectional pipe and writes the read end to `fds[0]` and the write
end to `fds[1]`. Pipes use a one-page ring buffer with `PIPE_BUF == 4096`.
Writes of `PIPE_BUF` bytes or fewer are atomic: a blocking writer waits until
the whole write can fit, and a nonblocking writer returns `-EAGAIN` without a
partial write if the whole request cannot fit. Larger writes may complete
partially.

Reads from an empty pipe block while a writer exists, return `-EAGAIN` when the
read end is nonblocking, and return `0` for EOF after all write ends close.
Writes fail with `-EPIPE` and deliver `SIGPIPE` after all read ends close.
`poll` and `epoll` report `POLLIN` for data or EOF, `POLLOUT` when space is
available, and `POLLHUP` after the peer side closes.

---

### SYS_PIPE2 (70)

```c
int sys_pipe2(int fds[2], uint32_t flags);
```

Creates a pipe with initial flags. Accepted flags are `O_NONBLOCK` and
`O_CLOEXEC`; other bits return `-EINVAL`.

---

### SYS_DUP (71), SYS_DUP2 (72), SYS_DUP3 (73)

```c
int sys_dup(int oldfd);
int sys_dup2(int oldfd, int newfd);
int sys_dup3(int oldfd, int newfd, uint32_t flags);
```

Duplicates descriptors in the POSIX shape. The new fd points at the same shared
open-file description, so file offsets and status flags such as `O_NONBLOCK`
are shared. Per-fd flags such as `FD_CLOEXEC` are independent: `dup` and
`dup2` clear close-on-exec on the new descriptor, while `dup3` accepts
`O_CLOEXEC`. `dup2(oldfd, oldfd)` succeeds as a no-op; `dup3(oldfd, oldfd)`
returns `-EINVAL`.

---

### SYS_FORK (74)

```c
int sys_fork(void);
```

Clones the current user process with eager address-space copying. The parent
receives the child pid, and the child resumes from the same syscall frame with
return value `0`. User memory is copied so later writes are independent. The fd
table is duplicated as descriptors pointing to the same shared open-file
descriptions, preserving shared file offsets and status flags.

---

### SYS_EXECVE (75)

```c
int sys_execve(const char* path, char* const argv[], char* const envp[]);
```

Replaces the current user image with a named ELF while preserving the process
pid, cwd, process group, and descriptors that do not have `FD_CLOEXEC` set.
The kernel validates and copies `path`, `argv`, and `envp` from user memory,
loads the program through a private PMM-backed image buffer, closes
close-on-exec descriptors, installs the new ELF address space, frees the
temporary image buffer, and returns to the new program entry point. Passing
`NULL` for `envp` inherits the caller's current environment.

The new program is entered as `_start(argc, argv, envp)`. `argv[argc]` and the
environment vector are both NULL-terminated.

### SYS_WAITPID_FG (76)

```c
int sys_waitpid_foreground(int pid, int* status);
```

Waits for a direct child while temporarily making the child's process group the
foreground owner of its terminal. This is the shell-facing wait path for
interactive foreground jobs.

### SYS_MEMINFO (77)

```c
int sys_meminfo(sys_meminfo_t* out_info);
```

Copies a kernel memory diagnostic summary into `out_info`, including the kernel
heap range, PMM free/total frame counts, and whether E820 boot memory metadata
is available. `pmm_total_frames` is the real managed frame count captured after
E820 filtering and boot reservations, not the fixed PMM address-window ceiling.
`ro_file_cache_pages` and `ro_file_cache_mapped_refs` expose diagnostic counts
for the shared read-only file mapping cache used by dynamic DSO text pages.

### SYS_E820_ENTRY (78)

```c
int sys_e820_entry(uint32_t index, sys_e820_entry_t* out_entry);
```

Copies one E820 memory-map entry by index. The return value is the total E820
entry count on success, `0` when no boot E820 map is available, `-EINVAL` for
an out-of-range index, or `-EFAULT` for an invalid output pointer.

### SYS_NETINFO (79)

```c
int sys_netinfo(sys_netinfo_t* out_info);
```

Copies the current NIC, IPv4, socket, and TCP diagnostic state. The NIC fields
include the active driver name, MAC/link state, TX/RX counters, error counters,
and selected hardware cursors/config registers. The IPv4
fields include whether an address is configured, address, netmask, gateway,
DNS server, DHCP server, and lease time. `/bin/ip`, `/bin/ipconfig`,
`netinfo`, and network smoke tests use this as the read side of runtime network
configuration.

### SYS_NET_OP (80)

```c
int sys_net_op(sys_net_op_request_t* req);
```

Performs narrow network maintenance or diagnostic operations. Supported `op`
values are:

| Operation | Behavior |
| --- | --- |
| `SYS_NET_OP_SEND_TEST_FRAME` | Queue a raw test frame on the active NIC. |
| `SYS_NET_OP_POLL_ONCE` | Drain at most one received network frame through the dispatcher. |
| `SYS_NET_OP_ARP` | Resolve `target_ip` or the configured gateway and write the next-hop MAC to `req->mac`. |
| `SYS_NET_OP_PING` | Send one ICMP echo to `target_ip` or the configured gateway. |
| `SYS_NET_OP_DHCP` | Request a DHCP IPv4 lease and install the returned address, netmask, gateway, DNS, DHCP server, and lease time. |
| `SYS_NET_OP_CONFIGURE` | Install a runtime static IPv4 config from `target_ip`, `netmask`, `gateway`, `dns`, `dhcp_server`, and `lease_seconds`. |
| `SYS_NET_OP_CLEAR_CONFIG` | Clear the runtime IPv4 config. |

The BusyBox `udhcpc` applet is patched to use `SYS_NET_OP_DHCP` for `eth0`,
matching the native `dhcp` and `ip dhcp` commands without opening raw packet
sockets.

Addresses are host-order IPv4 values in the same format used by the kernel
network drivers. These settings are runtime-only; they are not persisted to
disk.

### SYS_NET_IOCTL (145)

```c
int sys_net_ioctl(int fd, unsigned long request, void* arg);
```

Handles the Linux-shaped network ioctls used by classic BusyBox networking
applets. The kernel exposes one primary interface, `eth0`, backed by
`SYS_NETINFO`/`SYS_NET_OP` state. Supported requests include `SIOCGIFCONF`,
`SIOCGIFFLAGS`, `SIOCGIFADDR`, `SIOCGIFNETMASK`, `SIOCGIFBRDADDR`,
`SIOCGIFHWADDR`, `SIOCGIFMTU`, selected no-op setters, `SIOCSIFADDR`,
`SIOCSIFNETMASK`, `SIOCADDRT`, and `SIOCDELRT`. Route ioctls currently manage
the runtime default IPv4 gateway. `/proc/net/dev`, `/proc/net/route`, and
`/proc/net/arp` are rendered from the same state; header-only
`/proc/net/tcp`, `/proc/net/udp`, `/proc/net/raw`, and `/proc/net/unix` files
let BusyBox `netstat` run cleanly even before fuller socket table enumeration.

IPv6 and multi-interface behavior remain out of scope for this syscall slice.
Minimal rtnetlink is available through `AF_NETLINK` route sockets for BusyBox
`ip link`, `ip addr`, `ip route`, and `ip neigh`; `ip rule`, tunnel, and
advanced policy-route behavior remain unsupported. DNS-over-UDP is implemented
in libc on top of the UDP socket path when the runtime network state has a DNS
server, and the same UDP path is covered by BusyBox `tftp` client and
`udpsvd`/`tftpd` service smoke. BusyBox `udhcpc` is patched to call
`SYS_NET_OP_DHCP` for `eth0`; BusyBox `udhcpd` is built but not exercised by
the smoke suite.

### SYS_BLOCK_READ_SECTOR (81)

```c
int sys_block_read_sector(uint32_t lba, void* buf);
```

Copies one 512-byte sector from the mounted block device into a user buffer for
diagnostics. This follows the root filesystem's selected block backend, so the
same userspace tool can inspect ATA or USB storage. Returns `0` on success,
`-EIO` when no sector-backed root device is available or the read fails, or
`-EFAULT` for an invalid buffer.

`SYS_ATA_READ_SECTOR` remains a legacy alias for the same syscall number.

### SYS_EXEC_FG (82)

```c
int sys_exec_foreground(const char* name, int argc, char** argv);
```

Legacy spawn-style process creation for foreground commands. It launches the
named ELF in a new process group, inherits the caller's cwd and fd `0`/`1`/`2`,
claims the child for waiting, and returns the child pid.

### SYS_PTY_OPEN (83)

```c
int sys_pty_open(int fds[2], int master_flags);
```

Creates a pseudo-terminal pair. `fds[0]` receives the master fd and `fds[1]`
receives the slave fd. GUI shell windows keep the master and dup the slave onto
the child shell's standard descriptors.

### SYS_PTY_SET_SIZE (84)

```c
int sys_pty_set_size(int fd, uint32_t rows, uint32_t cols);
```

Updates the terminal dimensions associated with a PTY master or slave. Programs
that call `SYS_TERMINAL_SIZE` through the PTY slave observe these rows and
columns.

### Terminal Attribute and Ioctl Syscalls (129-131)

```c
int sys_tcgetattr(int fd, sys_termios_t* out);
int sys_tcsetattr(int fd, const sys_termios_t* in);
int sys_tty_ioctl(int fd, uint32_t request, void* arg);
```

Terminal attributes live in kernel terminal state. PTYs carry their own
termios data and console-like descriptors use a default console terminal
state. `sys_tty_ioctl` currently supports `TIOCGWINSZ`, `TIOCGPGRP`,
`TIOCSPGRP`, and compatibility no-ops for `TIOCSCTTY`/`TIOCNOTTY`;
non-terminal descriptors fail with `-ENOTTY`. The controlling-tty no-ops let
BusyBox `getty`/`login` tolerate SmallOS console and PTY descriptors without
modeling the full Linux tty-stealing lifecycle.

### Resource and Usage Syscalls (132-134)

```c
int sys_getrlimit(int resource, sys_rlimit_t* out);
int sys_setrlimit(int resource, const sys_rlimit_t* in);
int sys_getrusage(int who, sys_rusage_t* out);
```

`RLIMIT_NOFILE` is backed by the process fd limit and is enforced by fd
allocation and duplication paths. `RLIMIT_AS`, `RLIMIT_DATA`, and
`RLIMIT_STACK` report fixed values from the current user address-space layout,
and `RLIMIT_CPU` reports infinity. `getrusage(RUSAGE_SELF)` reports CPU ticks
as an approximate user time and task-owned RAM as `ru_maxrss`;
`RUSAGE_CHILDREN` reports accumulated waited-child ticks and waited-child count.

### Session and Process-Group Syscalls (135-138)

```c
int sys_setsid(void);
int sys_getsid(int pid);
int sys_setpgid(int pid, int pgid);
int sys_getpgid(int pid);
```

Sessions and process groups live in kernel process state. Children inherit the
parent session and process group; `setsid` creates a new session and process
group for non-group-leaders. `setpgid` can move the caller or one of its
children into a valid same-session process group, or create a new group whose
id is the target pid. Terminal foreground process-group ioctls validate that
the requested group exists in the caller's session. Terminal stop/continue
delivery uses those groups for Ctrl+Z, `killpg()`, BusyBox `ash` job control,
BusyBox `getty`/`login` terminal setup, and `waitpid(..., WUNTRACED)`
stopped-child reporting.

### Mount and Filesystem Statistics Syscalls (139-142)

```c
int sys_mount(const char* source,
              const char* target,
              const char* fstype,
              unsigned int flags,
              const void* data);
int sys_umount2(const char* target, unsigned int flags);
int sys_statfs(const char* path, sys_statfs_t* out);
int sys_fstatfs(int fd, sys_statfs_t* out);
```

The kernel owns a mount table seeded with the ext2 root, `/proc`, and `/dev`.
`/proc/mounts` is rendered from that table, and `statfs`/`fstatfs` report
per-mount filesystem identities: ext2 for `/`, proc for `/proc`, and devtmpfs
for `/dev`. `sys_mount` accepts dynamic `proc` and `devtmpfs` pseudo mounts on
existing ext2 directories and routes lookups below those mount points through
the same virtual `/proc` and `/dev` providers. Dynamic pseudo mounts can be
removed once no process cwd or open fd is below the target; busy unmounts fail
with `-EBUSY`. The static root, `/proc`, and `/dev` mounts remain pinned and
also fail `sys_umount2` with `-EBUSY`.

Unsupported filesystem types or unknown flags fail with `-EINVAL`.
Remount/bind/move/propagation action flags and dynamic ext2 stacking remain
out of scope for this milestone and fail with `-ENOSYS`.

### SYS_USB_MOUSE_OP (87)

```c
int sys_usb_mouse_op(uint32_t op, uint32_t port);
```

Temporary USB boot-mouse diagnostic session control. `op` is one of:

| Operation | Meaning |
| --- | --- |
| `SYS_USB_MOUSE_OP_OPEN` | Claim and initialize an OHCI boot mouse on `port`, or auto-scan when `port == 0`. Returns `1` when a mouse is opened, `0` when none is found, or a negative errno. |
| `SYS_USB_MOUSE_OP_POLL` | Poll the active USB boot mouse once and inject any relative report into the normal mouse state. Returns a positive event count, `0` for no event, or a negative errno. |
| `SYS_USB_MOUSE_OP_CLOSE` | Close the active diagnostic USB mouse session. |

This syscall is intentionally narrow: userland controls a diagnostic session,
but the OHCI driver and report injection remain kernel-owned.

### SYS_USBINFO (88)

```c
int sys_usbinfo(sys_usbinfo_t* out_info);
```

Copies the current USB controller, boot HID, USB storage, and last-probed OHCI
diagnostic counters into `out_info`. This backs `/bin/usbinfo` and returns
`0` on success or `-EFAULT` for an invalid output pointer.

### SYS_MOUSE_DEBUG (89)

```c
int sys_mouse_debug(sys_mousedebug_t* out_info);
```

Copies PS/2, VMware, and injected USB mouse diagnostic counters into
`out_info`, including IRQ/byte/packet counts, packet size, device id, init
state, and drop counters. This backs `/bin/mousetest` and returns `0` on
success or `-EFAULT` for an invalid output pointer.

### SYS_USB_DIAG_OP (90)

```c
int sys_usb_diag_op(uint32_t op, uint32_t arg);
```

Performs a narrow USB diagnostic action while keeping controller access inside
the kernel. Output formatting belongs to the user commands when the kernel can
return structured records. Supported `op` values:

| Operation | Meaning |
| --- | --- |
| `SYS_USB_DIAG_OP_PORTS` | Legacy kernel-print passive USB port dump. |
| `SYS_USB_DIAG_OP_DIAG` | Legacy kernel-print USB port/descriptor diagnostic path. |
| `SYS_USB_DIAG_OP_PEEK` | Run the OHCI address-0 descriptor peek for one-based port `arg`, used by `/bin/usbpeek`. |
| `SYS_USB_DIAG_OP_POWER` | Try OHCI root-hub port power and return the powered-port count, used by `/bin/usbpower`. |
| `SYS_USB_DIAG_OP_PORT_SNAPSHOT` | Copy a `sys_usb_port_snapshot_t` to `arg`; `/bin/usbports` and the passive part of `/bin/usbdiag` format this in userspace. |

`SYS_USB_DIAG_OP_PEEK` still performs active OHCI reset/control-transfer work
and prints descriptor details through the kernel terminal path.

The snapshot contains up to `SYS_USB_PORT_SNAPSHOT_MAX` entries. Controller
entries identify PCI address, programming interface, MMIO BAR, port count, and
controller-specific root-hub/capability words. Port entries carry the same
controller identity plus one-based port number and raw port status. If the
snapshot fills, `truncated` is set and userland should print the partial data.

### SYS_PROCINFO (91)

```c
int sys_procinfo(sys_procinfo_t* out_info);
```

Copies a scheduler/process diagnostic snapshot into `out_info`. The snapshot
contains the current tick count, the number of copied tasks, and one
`sys_procinfo_entry_t` per scheduled process up to `SYS_PROCINFO_MAX`. Each
entry includes pid, parent pid, process group, state, accumulated CPU timer
ticks, estimated task-owned RAM bytes, user heap bytes, and display name.
Stopped processes are reported as the `T` state in `/proc`-style views.

The RAM estimate is frame-based: `process_t`, kernel stack frames, dynamic fd
table frames, and private user page-directory/page-table/user frames. Kernel
mappings shared into every process are not charged per process. `/bin/top`
uses repeated snapshots to compute per-refresh CPU percentages and render the
live process table.

---

### SYS_DISPLAY_BLIT_STRIDE (92)

```c
int sys_display_blit_stride(uint32_t x, uint32_t y,
                            uint32_t w, uint32_t h,
                            uint32_t pitch_pixels,
                            const uint32_t* pixels);
```

Copies a rectangular XRGB8888 region from user memory into the framebuffer
using a source pitch measured in pixels. This is the preferred path for
presenting a sub-rectangle from a larger backbuffer without first packing it
into a temporary contiguous buffer. Like `SYS_DISPLAY_BLIT`, this keeps all
framebuffer pages coherent rather than presenting a hidden page. The caller
must hold the display via `SYS_DISPLAY_ACQUIRE`.

Returns `0` on success, `-EFAULT` for an invalid request or pixel buffer,
`-EINVAL` when `pitch_pixels < w`, `-EOVERFLOW` for impossible byte spans, or
`-EIO` if the caller does not own an active display.

---

### SYS_INPUT_WAIT_UNTIL (93)

```c
int sys_input_wait_until(uint32_t deadline_tick);
```

Blocks the calling process until either the shared input event queue becomes
nonempty or the absolute PIT tick deadline is reached. This lets event-loop
programs sleep until input or a frame deadline without repeatedly yielding. If
input is already pending, it returns immediately with `1`; if the deadline has
already passed, it returns `0`.

The wake path uses the same input queue as `SYS_INPUT_READ`, and timer wakeups
use the normal sleeping-process deadline path. The syscall does not consume
input events; callers should follow an input wake with `SYS_INPUT_READ`.

Returns `1` when input is pending, `0` when the deadline is reached first, or
`-EINVAL` if there is no current process.

---

### SYS_SOUND_OP (94)

```c
int sys_sound_op(uint32_t op, uint32_t arg1, uint32_t arg2);
```

Controls simple sound output. `SYS_SOUND_OP_TONE` plays `arg1` Hz for `arg2`
milliseconds, `SYS_SOUND_OP_PIT_DIVISOR` programs PIT channel 2 directly with
`arg1` for `arg2` milliseconds, `SYS_SOUND_OP_PIT_SEQUENCE` copies a bounded
pitch-byte sequence and advances it from IRQ0 at the requested sample rate,
`SYS_SOUND_OP_PCM_U8` plays an unsigned 8-bit mono PCM buffer through a
PCI AC97 bus-master stream when present, falling back to the Sound
Blaster-compatible 16-bit DMA path when AC97 is absent.
`SYS_SOUND_OP_PCM_U8_LEGACY` and
`SYS_SOUND_OP_PCM_U8_SB16_8` are diagnostic 8-bit DMA paths. `SYS_SOUND_OP_OPL_WRITE`
writes the 8-bit OPL2 register in `arg1` with the 8-bit value in `arg2`, and
`SYS_SOUND_OP_OPL_RESET` silences and reinitializes the AdLib-compatible OPL2
device. `SYS_SOUND_OP_CAPS` returns `SYS_SOUND_CAP_*` bits, `SYS_SOUND_OP_STATUS`
copies sound-driver diagnostic counters to `arg1`, and `SYS_SOUND_OP_STOP`
silences active PC speaker and PCM sound. OPL callers should use
`SYS_SOUND_OP_OPL_RESET` to silence FM synthesis. A tone duration of `0` leaves
it active until another sound operation or stop.

This stays intentionally small: it gives user programs a general beep/tone
surface, lets DOS ports such as Wolf3D play their original PC speaker pitch
data, exposes a bounded PCM path for digitized effects, and exposes raw OPL2
register writes for AdLib SFX/music sequencing.

The syscall starts or stops kernel-timed playback, but it is still mostly a
fire-and-forget surface. Ports that need precise "effect finished" semantics
currently keep their own monotonic deadlines in userland; Wolf3D's
level-completion intermission is the regression case that guards this boundary.

For structured operations, `arg1` points to the request structure:

```c
typedef struct sys_sound_pit_sequence {
    const unsigned char* samples;
    unsigned int count;
    unsigned int sample_hz;
    unsigned int divisor_scale;
} sys_sound_pit_sequence_t;

typedef struct sys_sound_pcm_u8 {
    const unsigned char* samples;
    unsigned int count;
    unsigned int sample_hz;
} sys_sound_pcm_u8_t;

typedef struct sys_sound_status {
    unsigned int caps;
    unsigned int pcm_active;
    unsigned int pcm_irq_count;
    unsigned int pcm_timeout_count;
    unsigned int pcm_error_count;
    unsigned int pcm_last_count;
    unsigned int pcm_last_hz;
} sys_sound_status_t;
```

Returns `0` on playback success, capability bits for `SYS_SOUND_OP_CAPS`,
`0` after copying `sys_sound_status_t` for `SYS_SOUND_OP_STATUS`, `-EFAULT`
for an invalid buffer pointer, `-EIO` when PCM or OPL hardware is not available
or cannot be programmed, or `-EINVAL` for an invalid operation, frequency, PIT
divisor, buffer length, sample rate, divisor scale, or OPL register/value.

---

### SYS_STAT_FULL (85)

```c
int sys_stat_full(const char* path, sys_stat_info_t* out_info);
```

Copies POSIX-shaped metadata for a path, following the final symbolic link.
The result includes device id, inode, mode/type bits, link count, uid/gid,
special-device id, size, block size, block count, timestamps, and the legacy
`is_dir` convenience flag. Virtual `/proc` and `/dev` entries also report
metadata through this shape.

Returns `0` on success, `-ENOENT` or another lookup errno for missing paths,
`-EFAULT` for invalid pointers, or `-ENAMETOOLONG` for paths that cannot be
normalized into the kernel path buffer.

---

### SYS_FSTAT_FULL (86)

```c
int sys_fstat_full(int fd, sys_stat_info_t* out_info);
```

Copies the same metadata shape for an open descriptor. ext2 file descriptors
report backing inode metadata; virtual, console, pipe, and socket-like handles
report their best available descriptor metadata.

Returns `0` on success, `-EBADF` for invalid descriptors, or `-EFAULT` for an
invalid output pointer.

---

### SYS_DISPLAY_MAP (95)

```c
int sys_display_map(sys_display_map_info_t* out_info);
```

Maps the framebuffer page-flip aperture into the owning process at
`SYS_DISPLAY_MAP_BASE` and copies mapped display geometry to `out_info`. The
caller must hold the display via `SYS_DISPLAY_ACQUIRE`, and the kernel only
enables this fast path when the active framebuffer can page flip. The mapped
format is currently XRGB8888.

`page_count` reports the number of VRAM pages available, `page_bytes` is the
byte span between pages, and `draw_page` is the hidden page the caller should
render next. Callers present a rendered page with `SYS_DISPLAY_PRESENT_PAGE`
instead of copying the whole frame through the kernel.

Returns `0` on success, `-EFAULT` for an invalid output pointer, or `-EIO` if
there is no current process, the caller does not own the display, or page
flipping is unavailable.

---

### SYS_DISPLAY_PRESENT_PAGE (96)

```c
int sys_display_present_page(sys_display_present_page_t* req);
```

Presents a page previously written through the `SYS_DISPLAY_MAP` mapping and
returns the next hidden page in `req->next_page`. `req->page` must be less than
the mapped `page_count`, and the caller must still own the display. The kernel
issues the required framebuffer write fence before changing scanout; on the BGA
framebuffer backend, the scanout change waits for a fresh vertical-retrace edge.

Returns `0` on success, `-EFAULT` for an invalid request pointer, or `-EIO` if
there is no current process, the requested page is out of range, the caller
does not own the display, or page flipping is unavailable.

---

### SYS_MMAP (97)

```c
void* sys_mmap(void* addr, uint32_t length, uint32_t prot, uint32_t flags,
               int fd, uint32_t offset);
```

Creates private user mappings. Anonymous mappings keep using `MAP_ANON`; file
mappings currently support only read-only/private mappings from a readable
regular file descriptor with a page-aligned `offset`. Writable file mappings
are rejected, and `mprotect(PROT_WRITE)` is rejected on shared read-only file
cache pages. Eligible file-backed pages are refcounted and may be shared across
processes and `fork()`. `PROT_EXEC` is accepted for ABI shape, but current x86
paging only enforces present/user/write bits.

### SYS_MUNMAP (98)

```c
int sys_munmap(void* addr, uint32_t length);
```

Unmaps pages from a user mapping range. Partial-page addresses are rounded to
page boundaries by the kernel.

### SYS_MPROTECT (99)

```c
int sys_mprotect(void* addr, uint32_t length, uint32_t prot);
```

Updates read/write protection on user mapping pages. Execute permission is
accepted but not separately enforced on current paging hardware.

---

### SYS_LINK (100)

```c
int sys_link(const char* oldpath, const char* newpath);
```

Creates a hard link to an existing non-directory ext2 inode and increments its
link count. Existing destinations fail with `-EEXIST`.

---

### SYS_SYMLINK (101)

```c
int sys_symlink(const char* target, const char* linkpath);
```

Creates a symbolic link inode whose payload is `target`. The target string is
stored as given and is not required to exist at creation time.

---

### SYS_READLINK (102)

```c
int sys_readlink(const char* path, char* out, uint32_t out_size);
```

Reads the target of a symbolic link without following the final component.
Like POSIX `readlink`, the returned byte count is not a promise of a trailing
NUL in the caller's buffer.

---

### SYS_LSTAT_FULL (103)

```c
int sys_lstat_full(const char* path, sys_stat_info_t* out_info);
```

Copies full metadata without following the final symbolic link. Intermediate
symlinks are still resolved during path traversal.

---

### SYS_CHMOD (104), SYS_FCHMOD (109)

```c
int sys_chmod(const char* path, uint32_t mode);
int sys_fchmod(int fd, uint32_t mode);
```

Updates stored ext2 permission bits while preserving the inode type bits.
The caller must be root or own the target inode. The kernel enforces these
mode bits for path traversal, open/create/remove/link operations, exec, and
metadata updates.

---

### SYS_CHOWN (105), SYS_FCHOWN (110)

```c
int sys_chown(const char* path, uint32_t uid, uint32_t gid);
int sys_fchown(int fd, uint32_t uid, uint32_t gid);
```

Updates stored ext2 owner and group ids.

---

### SYS_UTIMENS (106), SYS_FUTIMENS (111)

```c
int sys_utimens(const char* path, const struct timespec times[2]);
int sys_futimens(int fd, const struct timespec times[2]);
```

Updates access and modification timestamps in seconds. Passing `NULL` uses the
current realtime value for both fields. Nanoseconds are validated for POSIX
shape but ext2 stores whole seconds.

The caller must be root, own the target inode, or have write permission on the
target file.

---

### SYS_GETUID (112), SYS_GETEUID (113), SYS_GETGID (114), SYS_GETEGID (115)

```c
uint32_t sys_getuid(void);
uint32_t sys_geteuid(void);
uint32_t sys_getgid(void);
uint32_t sys_getegid(void);
```

Returns the caller's kernel-owned real/effective uid or gid. Processes start as
root at boot and inherit credentials across fork/exec.

---

### SYS_SETUID (116), SYS_SETGID (117)

```c
int sys_setuid(uint32_t uid);
int sys_setgid(uint32_t gid);
```

Changes the caller's real and effective uid or gid. Root can switch to any id.
Non-root callers may only set the id to their current real or effective id.
SmallOS does not yet model saved ids, setuid/setgid inode bits, or full
supplementary-group transitions; password verification is handled in userland
by native `/bin/login` and `/bin/passwd` against `/etc/shadow`.

---

### SYS_UMASK (118)

```c
uint32_t sys_umask(uint32_t mask);
```

Sets the process `umask` and returns the previous value. The kernel applies the
mask in create paths for regular files, directories, and special nodes.

---

### SYS_OPEN_CREATE_MODE (119)

```c
int sys_open_mode_create(const char* path, uint32_t flags, uint32_t create_mode);
```

Mode-aware open with an explicit POSIX create mode for `O_CREAT` callers. The
`flags` argument uses the same `SYS_OPEN_MODE_*` bits as `SYS_OPEN_MODE`.
Existing files are checked against the requested read/write intent; new files
require write+execute permission on the parent directory, receive
`create_mode & ~umask`, and are owned by the caller's effective uid/gid.

---

### Directory-Fd Syscalls (120-128)

```c
int sys_openat_mode_create(int dirfd, const char* path,
                           uint32_t flags, uint32_t create_mode);
int sys_fstatat_full(int dirfd, const char* path,
                     sys_stat_info_t* out, uint32_t flags);
int sys_unlinkat(int dirfd, const char* path, uint32_t flags);
int sys_mkdirat(int dirfd, const char* path, uint32_t mode);
int sys_renameat(int olddirfd, const char* oldpath,
                 int newdirfd, const char* newpath);
int sys_linkat(int olddirfd, const char* oldpath,
               int newdirfd, const char* newpath, uint32_t flags);
int sys_symlinkat(const char* target, int newdirfd, const char* linkpath);
int sys_readlinkat(int dirfd, const char* path, char* out, uint32_t out_size);
int sys_utimensat(int dirfd, const char* path,
                  const struct timespec times[2], uint32_t flags);
```

These syscalls resolve relative paths against `dirfd`, or against the caller's
cwd when `dirfd == AT_FDCWD`. Absolute paths ignore `dirfd`. Non-directory base
fds fail with `ENOTDIR`; invalid fds fail with `EBADF`.

`SYS_FSTATAT_FULL` and `SYS_UTIMENSAT` support `AT_SYMLINK_NOFOLLOW`.
`SYS_UNLINKAT` supports `AT_REMOVEDIR`. `SYS_LINKAT` accepts
`AT_SYMLINK_FOLLOW` for BusyBox/POSIX-shaped callers; unsupported flag bits
return `EINVAL`.

---

### SYS_MKNOD (107)

```c
int sys_mknod(const char* path, uint32_t mode, uint32_t dev);
```

Creates an ext2 regular or special inode. FIFO, character, block, and socket
nodes are stat-visible for BusyBox/POSIX compatibility; character and block
nodes store `dev` as their reported special-device id.

---

### SYS_FTRUNCATE (108)

```c
int sys_ftruncate(int fd, uint32_t size);
```

Resizes a writable regular ext2 file descriptor. Shrinking frees blocks beyond
the new end; growing zero-fills the newly exposed range.

---

## Kernel Entry Point

```c
void syscall_handler_main(syscall_regs_t* regs);
```

---

## Register Frame Layout

This **must match** `isr128_stub`.

Assembly push order:

```asm
pusha
push ds
push es
push fs
push gs
push esp   ; pointer passed to C
```

C struct (fields in reverse push order — last pushed is at lowest address):

```c
typedef struct syscall_regs {
    unsigned int gs;
    unsigned int fs;
    unsigned int es;
    unsigned int ds;
    unsigned int edi;
    unsigned int esi;
    unsigned int ebp;
    unsigned int esp;
    unsigned int ebx;
    unsigned int edx;
    unsigned int ecx;
    unsigned int eax;
} syscall_regs_t;
```

---

## ⚠️ Critical Rule

If you change **anything** in `isr128_stub`, you MUST update `syscall_regs_t` to match. Failure silently corrupts arguments and return values.

---

## Userspace Helpers

`src/user/internal/user_syscall.h`:

```c
sys_write(buf, len)
sys_exit(code)
sys_get_ticks()
sys_putc(c)
sys_read(buf, len)
sys_yield()
sys_exec(name, argc, argv)
sys_open(name)
sys_close(fd)
sys_fread(fd, buf, len)
sys_sleep(ticks)
sys_writefile(name, buf, len)
sys_halt()
sys_reboot()
sys_writefile_path(path, buf, len)
sys_brk(new_brk)
sys_open_write(name)
sys_writefd(fd, buf, len)
sys_lseek(fd, offset, whence)
sys_unlink(path)
sys_rename(src, dst)
sys_stat(path, out_size, out_is_dir)
sys_socket(domain, type, protocol)
sys_bind(fd, addr, addrlen)
sys_listen(fd, backlog)
sys_accept(fd, addr, addrlen)
sys_connect(fd, addr, addrlen)
sys_send(fd, buf, len)
sys_recv(fd, buf, len)
sys_poll(fds, nfds, timeout)
sys_mkdir(path, mode)
sys_rmdir(path)
sys_dirlist(path, index, out)
sys_setsockopt(fd, level, optname, optval, optlen)
sys_getsockname(fd, addr, addrlen)
sys_open_mode(name, mode)
sys_getcwd(buf, size)
sys_chdir(path)
sys_fsync(fd)
sys_read_raw(buf, len)
sys_fcntl(fd, cmd, arg)
sys_epoll_create(flags)
sys_epoll_ctl(epfd, op, fd, event)
sys_epoll_wait(epfd, events, maxevents, timeout)
sys_timerfd_create(clock_id, flags)
sys_timerfd_settime(fd, flags, new_value, old_value)
sys_signalfd(fd, mask, flags)
sys_accept4(fd, addr, addrlen, flags)
sys_shutdown(fd, how)
sys_getpeername(fd, addr, addrlen)
sys_fstat(fd, out_size, out_is_dir)
sys_terminal_size(out_rows, out_cols)
sys_display_info(out_info)
sys_display_fill(x, y, w, h, color)
sys_display_blit(x, y, w, h, pixels)
sys_display_acquire()
sys_display_release()
sys_display_blit_stride(x, y, w, h, pitch_pixels, pixels)
sys_display_map(out_info)
sys_display_present_page(req)
sys_mouse_read(out_state)
sys_input_read(out_events, max_events, flags)
sys_fsinfo(out_info)
sys_fsmap(req)
sys_getpid()
sys_waitpid(pid, status, options)
sys_kill(pid, signum)
sys_dirlist_batch(path, index, out, max_count)
sys_clock_gettime(clock_id, ts)
sys_clock_settime(clock_id, ts)
sys_ntp_sync(server_ip, out_ts)
sys_pipe(fds)
sys_pipe2(fds, flags)
sys_dup(oldfd)
sys_dup2(oldfd, newfd)
sys_dup3(oldfd, newfd, flags)
sys_fork()
sys_execve(path, argv, envp)
sys_waitpid_foreground(pid, status)
sys_meminfo(out_info)
sys_procinfo(out_info)
sys_e820_entry(index, out_entry)
sys_netinfo(out_info)
sys_net_op(req)
sys_block_read_sector(lba, buf)
sys_exec_foreground(name, argc, argv)
sys_pty_open(fds, master_flags)
sys_pty_set_size(fd, rows, cols)
sys_tcgetattr(fd, out)
sys_tcsetattr(fd, in)
sys_tty_ioctl(fd, request, arg)
sys_getrlimit(resource, out)
sys_setrlimit(resource, in)
sys_getrusage(who, out)
sys_setsid()
sys_getsid(pid)
sys_setpgid(pid, pgid)
sys_getpgid(pid)
sys_mount(source, target, fstype, flags, data)
sys_umount2(target, flags)
sys_statfs(path, out)
sys_fstatfs(fd, out)
sys_stat_full(path, out_info)
sys_fstat_full(fd, out_info)
sys_sound_op(op, arg1, arg2)
sys_sound_tone(frequency_hz, duration_ms)
sys_sound_pit_divisor(divisor, duration_ms)
sys_sound_pit_sequence(samples, count, sample_hz, divisor_scale)
sys_sound_pcm_u8(samples, count, sample_hz)
sys_sound_pcm_u8_legacy(samples, count, sample_hz)
sys_sound_pcm_u8_sb16_8(samples, count, sample_hz)
sys_sound_caps()
sys_sound_status(out)
sys_sound_stop()
sys_mmap(addr, length, prot, flags)
sys_munmap(addr, length)
sys_mprotect(addr, length, prot)
sys_link(oldpath, newpath)
sys_symlink(target, linkpath)
sys_readlink(path, out, out_size)
sys_lstat_full(path, out_info)
sys_chmod(path, mode)
sys_chown(path, uid, gid)
sys_utimens(path, times)
sys_mknod(path, mode, dev)
sys_ftruncate(fd, size)
sys_fchmod(fd, mode)
sys_fchown(fd, uid, gid)
sys_futimens(fd, times)
```

`src/user/internal/user_lib.h` higher-level wrappers:

```c
u_puts(...)        sys_write wrapper
u_putc(...)        sys_putc wrapper
u_put_uint(...)    decimal integer output
u_readline(...)    sys_read + null-terminate + strip newline
u_open_write(...)  streaming write/truncate open
u_writefd(...)     write to writable handle
u_lseek(...)       reposition writable handle
u_unlink(...)      remove ext2 file
u_rename(...)      rename or move ext2 entry
u_stat(...)        query path metadata
```

---

## Design Notes

* Programs run in ring 3 — hardware-enforced privilege separation
* All pointer arguments to syscalls are validated with page-aware user checks before kernel dereference — pointers below `USER_CODE_BASE` (0x400000), spanning above `USER_STACK_TOP` (0xC0000000), touching an unmapped user page, or wrapping a variable-length byte count are rejected before use
* `SYS_EXEC` copies the user `name` and `argv[]` strings into kernel buffers before handing them to the ELF loader, so the loader never depends on caller memory staying stable after validation
* Each process owns a cwd. Kernel path syscalls normalize `.` / `..`, accept absolute paths with a leading slash, and resolve relative paths against that process cwd before entering VFS or ELF loading.
* Timer IRQ scheduler handoff uses the adjusted C-call resume frame from `irq0_handler_main`, currently `esp - 8`; `SYS_YIELD` itself now parks with `sti; hlt; cli` and lets interrupt handling decide whether a scheduler switch is needed.
* EOI for IRQ1 is sent at the top of `irq1_handler_main` before `keyboard_handle_irq`; IRQ12 sends EOI to both PICs before decoding PS/2 mouse bytes or draining VMware mouse events
* The TSS is owned by the GDT subsystem. Syscall entry uses the currently active `SS0/ESP0`, and scheduler-driven updates to ESP0 go through `tss_set_kernel_stack()` rather than a cached pointer into the packed TSS.
* fd 0/1/2 are real console handles created by `process_create()` (`stdin`, `stdout`, `stderr`) and may be replaced by inherited PTY slave handles for GUI shell sessions; user-opened files, pipes, sockets, and event handles start at fd 3. The descriptor table is PMM-backed process state: it starts at 16 slots, grows up to the default 128-fd process limit, and has a kernel hard cap of 256. Every handle carries readable/writable state plus an ops table for `read`, `write`, `seek`, `poll`, `flush`, and `close`. File, pipe, PTY, and socket resources are shared/refcounted when descriptors are duplicated or inherited across `fork()`, while `FD_CLOEXEC` is per descriptor. `process.c` owns fd lifetime and dispatch, `vfs.c` owns ext2-backed file behavior, `socket.c` owns kernel socket objects plus accept/read/write wait queues, and `tcp.c` owns passive TCP listeners, the global 4-tuple connection table, and the lazy RX/TX rings behind connected sockets.
* `SYS_WRITEFILE` is the simplest root-only persistence path for user tools that want to emit a generated artifact without managing an fd-based write stream.
* `SYS_WRITEFILE_PATH` is the preferred path-aware persistence primitive for compilers and build tools, especially when writing into nested directories.

---

## Future Extensions

* `SYS_ALLOC`

---

## Debugging Tips

If syscalls stop working: check `isr128_stub`, check `syscall_regs_t`, verify register order. Test with `SYS_PUTC` first. Common symptom of mismatch: garbage output, only one character prints, crashes after `int 0x80`.
