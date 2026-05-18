# User Runtime

This document describes the user-space runtime contract for SmallOS ELF
programs. It covers the boundary between raw syscalls, POSIX-like wrappers,
stdio streams, directory traversal, and the hosted-ish expectations used by
TinyCC.

---

# Runtime Layers

SmallOS user programs are freestanding ELF binaries linked with the SmallOS
runtime objects from `src/user/`.

The layers are:

```text
program code
  -> POSIX-ish wrappers: open/read/write/stat/access/realpath/opendir/fopen
  -> raw syscall helpers in user_syscall.h
  -> int 0x80 syscall ABI
  -> kernel syscall dispatcher
  -> VFS / ext2 / console / socket backends
```

Programs may call raw `sys_*` helpers directly, but higher-level code should
prefer the POSIX-like wrappers when it wants normal `errno` behavior.

---

# Syscalls And Errno

Raw syscalls return SmallOS kernel results directly:

- non-negative values mean success
- negative values mean `-errno`

For example, `sys_open("missing")` returns `-ENOENT`.

POSIX-like wrappers translate raw syscall errors to:

```text
return -1
errno = positive errno value
```

For example, `open("missing", O_RDONLY)` returns `-1` and sets
`errno = ENOENT`.

This split is intentional. Low-level probes such as `ptrguard`, `badptrprobe`,
and `fileread` exercise raw syscall results, while hosted-ish code such as
TinyCC relies on the wrapper convention.

---

# Cwd And Paths

Each process has a current working directory stored in its `process_t`.

Kernel path-taking syscalls resolve paths against that cwd before touching the
filesystem. Absolute paths start at the ext2 root. Relative paths start at the
calling process cwd.

Path normalization supports:

- repeated separators
- `.`
- `..`
- root clamping when `..` would walk above `/`

Userland `realpath(path, resolved)` mirrors this normalization for programs
that compare paths before opening them. It returns canonical absolute paths
with a leading slash, such as:

```text
cwd: /usr/bin
realpath("./hello.elf") -> /usr/bin/hello.elf
```

Current runtime limits are intentionally small:

- canonical paths fit in 128 bytes
- components fit in 31 bytes
- normalization supports up to 16 path components

---

# File Descriptors

Every process has a dynamic descriptor table in `process_t`. Each fd entry is
a descriptor that points at a shared open-file description for files, pipes,
and sockets. `dup*()` and `fork()` copy descriptor entries while preserving the
shared offset/status state; `FD_CLOEXEC` remains per descriptor and is honored
by `execve()`.

Descriptor layout:

```text
0  stdin   console or PTY read
1  stdout  console or PTY write
2  stderr  console or PTY write
3+ user-opened files, pipes, sockets, and event handles
```

Interactive full-screen programs can use `sys_read_raw()` when they need
keyboard input without kernel echo. Foreground process input reports ordinary
characters plus ANSI-style special-key sequences for arrows, Home/End,
Delete, PageUp/PageDown, and function keys; `edit` uses this path together
with normal fd-backed writes to edit ext2 text files.
Programs that redraw a whole screen, such as `top`, combine raw reads with
nonblocking `sys_poll()` on fd `0` and ANSI cursor/screen control written to fd
`1`. That keeps live tools responsive to single-key commands such as `q`
without waiting for a newline.

PTY-backed GUI shells use the same descriptor contract. The GUI owns the PTY
master, the shell process inherits the slave on fd `0`/`1`/`2`, and child
programs launched by the shell inherit those descriptors unless marked
close-on-exec.

Graphics programs that need mouse motion can call `sys_mouse_read()` from
`user_syscall.h`. It returns accumulated relative movement and button bits from
PS/2, VMware absolute-pointer translation, or the OHCI USB boot mouse path, then
clears the movement counters. This is a raw polling helper, not a
descriptor-backed event stream.

USB and mouse diagnostic commands use the same raw syscall layer instead of
running as kernel built-ins. `sys_usbinfo()`, `sys_mouse_debug()`,
`sys_usb_port_snapshot()`, `sys_usb_diag_op()`, and `sys_usb_mouse_op()` expose
snapshots or explicit diagnostic actions for `/bin/usb*.elf` and
`/bin/mousetest.elf`. `usbports` and the passive part of `usbdiag` format
`sys_usb_port_snapshot_t` records in userspace; active USB peeks, controller
access, and report injection remain kernel-owned.

The kernel dispatches descriptors through per-handle ops:

```text
read / write / seek / poll / flush / close
```

Socket descriptors point at kernel `socket_t` objects; blocking socket reads,
accepts, and socket-backed `poll`/`epoll_wait` waits use socket-owned
accept/read/write wait queues. Timerfd/signalfd-style handles have their own
read wait queues, and expired timerfds wake waiters from the timer IRQ path.
Pipe descriptors point at a refcounted one-page ring buffer. Reads block on
empty pipes while writers exist, writes block on full pipes while readers
exist, `PIPE_BUF` is 4096 bytes, and `poll`/`epoll` report pipe readability,
writability, and hangup through the generic handle path.
Accepted TCP streams are tracked in a global
4-tuple TCP table, allocate a 64 KiB PMM-backed receive ring on first payload,
and release it again after userland drains the buffer. Socket writes allocate a
16 KiB TX ring on first payload, keep queued bytes until ACKed, release the ring
once drained, and wake write waiters as TX space returns.

ext2-backed file descriptors support:

- read-only opens
- write/create/truncate opens
- append opens
- read/write opens
- seek
- flush
- close-time writeback

Write-capable ext2 operations require a writable mount. Normal ATA boots are
writable; USB mass-storage boots currently mount `usb0` read-only, so create,
truncate, append, rename, unlink, and directory mutation calls fail through the
ordinary negative-errno paths instead of persisting changes to the stick.

The user-visible fd API is preserved while file writes stream directly through
ext2 write-at. File offsets, status flags, and cached read data live on the
shared file description, so duplicated and fork-inherited regular-file
descriptors share offsets as POSIX code expects.

Current fd-backed regular files are bounded by ext2 free space and the ext2
driver's safety limit for the 16 MB test volume. The older whole-file
`ext2_load()` helper still has a 1 MB static-buffer limit, so runtime file IO
should prefer descriptors when it needs seek, append, writes, or larger
readback.

---

# POSIX-Like Wrappers

The runtime provides a small POSIX-shaped surface:

- `open`, `close`
- `read`, `write`
- `pipe`, `pipe2`
- `dup`, `dup2`, `dup3`
- `lseek`
- `stat`, `lstat`, `fstat`
- `access`
- `unlink`, `remove`
- `rename`
- `mkdir`, `rmdir`
- `getcwd`, `chdir`
- `getpid`, `fork`, `execve`, `execv`, `execvp`, `waitpid`, `kill`
- `environ`, `getenv`, and `main(argc, argv, envp)` through `user_crt0`
- `time`, `gettimeofday`, `clock_gettime`, `clock_settime`
- socket, poll, epoll, timerfd, and signalfd wrappers used by guest services

`access(path, mode)` validates mode bits and checks existence through
`SYS_STAT`. SmallOS does not currently model Unix permission bits, so `R_OK`,
`W_OK`, and `X_OK` are existence/type checks rather than permission checks.

`stat`, `lstat`, and `fstat` use the full stat syscalls and fill inode number,
mode bits, link count, uid/gid, size, block size, block count, and ext2
timestamps. Newly created ext2 files and directories currently default to
`0644` and `0755` respectively.

`execve(path, argv, envp)` copies both argument and environment vectors into
kernel-owned storage before replacing the image. Passing `NULL` for `envp`
inherits the caller's current environment. `execv` and `execvp` use `environ`,
and `execvp` searches `PATH`, falling back to `/bin:/usr/bin:/usr/sbin`.

---

# Time

The runtime exposes `CLOCK_MONOTONIC` as uptime and `CLOCK_REALTIME` as a
settable wall clock. The kernel stores realtime as an offset from uptime, so
the clock continues advancing after boot-time or manual synchronization.

Boot queues DHCP configuration and then a best-effort NTP sync through the
active NIC/IPv4/UDP path while the startup splash is visible. `/bin/date`
prints the current UTC realtime value, and `date -s [server-ip]` asks the
kernel NTP helper to synchronize again. The default server is `129.6.15.28`.

`time()` and `gettimeofday()` use `CLOCK_REALTIME`. `clock_gettime()` accepts
`CLOCK_REALTIME` and `CLOCK_MONOTONIC`; `clock_settime()` accepts
`CLOCK_REALTIME`.

---

# Stdio

`FILE` streams are fd-backed runtime objects.

Supported operations include:

- `fopen`, `fdopen`, `freopen`, `fclose`
- `fread`, `fwrite`
- `fgetc`, `fgets`, `getc`, `getchar`
- `fputc`, `fputs`, `putchar`, `puts`
- `ungetc`
- `fflush`
- `feof`, `ferror`, `clearerr`
- `fseek`, `ftell`
- `printf` family helpers

The implementation is deliberately unbuffered at the user-runtime level:
`fread` and `fwrite` call into fd syscalls directly. Stream state is still
tracked normally:

- EOF is set when a read attempts to read past the end of file.
- Error is set for invalid stream operations and syscall failures.
- `clearerr` clears both EOF and error bits.
- `ungetc` clears EOF for the pushed-back byte.

`fflush(stream)` calls `SYS_FSYNC` for writable file streams. That makes it
meaningful even though userland stdio is unbuffered: it asks the kernel VFS
layer to commit any dirty writable descriptor state. ext2 fd writes now stream
to disk as they arrive on writable storage, so `fflush` is usually a
confirmation point rather than a whole-file rewrite. Console streams treat
`fflush` as success.

---

# Directory Traversal

Directory traversal is provided through:

```c
DIR *opendir(const char *path);
struct dirent *readdir(DIR *dirp);
int closedir(DIR *dirp);
```

The implementation uses `SYS_STAT` to validate that `opendir` targets an
existing directory, then uses `SYS_DIRLIST` for iteration.

`SYS_DIRLIST` is currently index-based: each `readdir()` asks the kernel for
entry `N` by path. That keeps the ABI small, but large directory walks can
re-scan earlier entries. A future streaming directory handle or fd-backed
directory iterator would be the right shape for faster tools such as `tree`.

Current `struct dirent` contains:

```c
char d_name[NAME_MAX + 1];
unsigned int d_size;
int d_is_dir;
```

ext2 display names follow the existing filesystem presentation rules:

- short native names are returned in display form
- directory names include a trailing `/`
- iteration returns `NULL` at EOF
- invalid handles set `errno = EBADF`

FTP uses the same public directory runtime as other user programs, so FTP
directory listing behavior should stay aligned with `dirprobe`.

---

# TinyCC Expectations

`usr/bin/tcc.elf` is built from TinyCC submodule sources plus the SmallOS runtime.
It links `src/user/user_crt0.c`, so the kernel still enters `_start(argc, argv)`
while TinyCC itself runs through its upstream `main(argc, argv)` path. It relies
on normal runtime behavior:

- cwd-aware file opens
- normalized path handling through `realpath`
- fd-backed stdio streams
- meaningful `fflush`
- directory traversal through `opendir` / `readdir`
- ext2 writeback through normal streaming fd writes and flush/close when the mounted storage is writable

The TinyCC acceptance gate is the guest compiler suite:

```text
tinycc_math
tinycc_agg
tinycc_tree
tinycc_compile
```

Any runtime or filesystem change should keep those tests passing unless the
change explicitly updates the TinyCC contract and tests in the same patch.

The generic CRT adapter is also the preferred startup path for new hosted-ish
SmallOS user programs:

```c
int main(int argc, char** argv);
```

The kernel launches `void _start(int argc, char** argv, char** envp)`.
`user_crt0` provides that symbol, sets global `environ`, calls
`main(argc, argv, envp)`, and passes the return value to `sys_exit`.
Two-argument `main(argc, argv)` programs continue to work because the extra
cdecl argument is ignored by callees that do not declare it. Direct
`_start(argc, argv)` remains available for low-level probes; the extra stack
argument is harmless there as well. `argv[argc]` and the environment vector are
both guaranteed to be `NULL` terminated.

---

# Tests

Runtime coverage currently lives in guest ELF probes:

- `fileread` - raw fd read/EOF/bad-fd behavior
- `fileprobe` - POSIX open modes, large write/readback, seek, append, partial-sector writes, zero-filled gaps, rename/delete
- `cwdprobe` - process cwd, relative opens, `realpath`, `access`
- `statprobe` - `SYS_STAT`, POSIX `stat`, `access`
- `stdioprobe` - EOF/error state, `clearerr`, `fflush`, invalid stdio ops
- `dirprobe` - root and nested directory iteration, EOF, invalid/missing dirs
- `errnoprobe` - wrapper `errno` behavior
- `crtprobe` - `main(argc, argv)` via `user_crt0`, argv terminator, and return status
- `waitprobe` - `SYS_EXEC` pid return, `waitpid`, `WNOHANG`, `kill`, and wait status macros
- `pipeprobe` - pipe read/write, EOF, `EPIPE`, nonblocking behavior, `PIPE_BUF`, poll readiness, and blocking transfer wakeups
- `dupprobe` - `dup`, `dup2`, shared file offsets/status flags, and independent `FD_CLOEXEC`
- `forkprobe` - parent/child return values, copied memory independence, `waitpid`, and inherited shared file offsets
- `execveprobe` - replacing `execve` image handoff and argv delivery

Run the full acceptance suite with:

```bash
make test
```

For docs-only changes, a lightweight sanity check is:

```bash
make
rg "user-runtime" docs
rg "SYS_FSYNC|stdio|dirent|TinyCC" docs/user-runtime.md
```
