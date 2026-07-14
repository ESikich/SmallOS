# User Runtime

This document describes the user-space runtime contract for SmallOS ELF
programs. It covers the boundary between raw syscalls, POSIX-like wrappers,
stdio streams, directory traversal, and the hosted-ish expectations used by
TinyCC.

---

# Runtime Layers

SmallOS user programs are freestanding ELF binaries linked with SmallOS user
libraries built from `src/user/`: `libc.a`, `libm.a`, and `libposix.a`, or
dynamic executables linked against the combined `/lib/libc.so` runtime.
Hosted-ish programs also link `src/user/crt/crt0.c`.

Public user headers live under `src/user/include/`. Raw syscall and native
runtime helper headers live under `src/user/internal/`; normal ports should
prefer the public libc/POSIX headers and only include internal headers when
they intentionally target SmallOS-specific low-level behavior.

The seed filesystem installs the same public surface for guest builds:
`/usr/include` contains libc/POSIX/SmallOS headers plus kernel UAPI headers,
and `/usr/lib` contains `crt0.o`, `libc.a`, `libm.a`, and `libposix.a`.
That lets in-guest compilers target the normal hosted `main()` path without
knowing about repository-internal runtime files.

Runtime dynamic-linking artifacts live outside that guest build sysroot:
`/lib/ld-smallos.so` is the interpreter named by dynamic executables, and
`/lib/libc.so` is the combined shared libc/POSIX/libm runtime loaded by the
interpreter. The static archives remain supported indefinitely for TinyCC and
for commands that are not yet part of the dynamic conversion wave.

The layers are:

```text
program code
  -> POSIX-ish wrappers: open/read/write/stat/access/realpath/opendir/fopen
  -> raw syscall helpers in src/user/internal/user_syscall.h
  -> int 0x80 syscall ABI
  -> kernel syscall dispatcher
  -> VFS / ext2 / console / socket backends
```

Programs may call raw `sys_*` helpers directly, but higher-level code should
prefer the POSIX-like wrappers when it wants normal `errno` behavior.

---

# Dynamic Runtime Notes

Dynamic executables use the same public headers and libc/POSIX APIs as static
programs. The current loader supports eager i386 relocations needed by the
runtime, including copy relocations for process globals such as `stdout`,
`stderr`, `errno`, and `environ`. That means code using normal stdio and errno
should behave the same whether the program was staged from `foo.elf`,
`foo.dyn.elf`, or `foo.pie.elf`.

The v2 loader is a self-relocating `ET_DYN` interpreter. Legacy dynamic
executables remain fixed-address `ET_EXEC`; selected command and crt0 probe
waves plus the explicit PIE probes are base-zero `ET_DYN` main executables
mapped at deterministic `USER_PIE_BASE`. The loader derives the main load bias
from auxv and program headers before relocating the main object. It maps
eligible read-only DSO file pages through the kernel's shared read-only file
cache, so `/lib/libc.so` text can share physical frames across dynamic
processes.
Writable DSO data, BSS, GOT/relocation-bearing pages, and mixed tail pages
remain private per process. Startup dependency lookup honors absolute
`DT_NEEDED` paths, absolute-only `DT_RUNPATH`/`DT_RPATH`, and the default
`/lib` fallback.

Dynamic programs also get the public `<dlfcn.h>` surface through a loader
service table installed into `/lib/libc.so` before program entry. `dlopen()`
supports `RTLD_NOW`, treats `RTLD_LAZY` as eager binding, accepts
`RTLD_GLOBAL` by adding active runtime objects to `RTLD_DEFAULT` lookup, and
keeps `RTLD_LOCAL` runtime objects visible only through their own handle and
dependency closure. `dlsym(RTLD_DEFAULT, name)` searches the active global
load order, while object handles search that object and its dependency
closure. `dlclose()` drops runtime references and runs finalizers when the last
runtime reference goes away, but V2 intentionally does not unmap or reclaim DSO
pages.
Inactive runtime objects are hidden from `RTLD_DEFAULT` lookup, stale handles
fail while inactive, and runtime load failures are reported through
`dlerror()` without killing the process. Runtime DSOs are placed in a bounded
loader-owned mmap arena below the interpreter. Static programs still report
unsupported `dlfcn` operations. Lazy PLT binding, TLS, `RTLD_NEXT`, symbol
versioning, and aggressive unload/reclamation remain out of scope.

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

`strerror()` is part of the public libc string surface, declared by
`<string.h>`, and maps shared SmallOS errno values to stable diagnostic text.
`perror()` writes that text through fd-backed `stderr`.

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
realpath("./hello") -> /usr/bin/hello
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

Interactive full-screen programs can use the public `term_keys.h` helper when
they need keyboard input without kernel echo. Foreground process input reports
ordinary characters plus ANSI-style special-key sequences for arrows,
Home/End, Delete, PageUp/PageDown, and function keys. `term_keys.h`, backed by
`libc.a`, turns those byte sequences into stable `TERM_KEY_*` values and owns
the nonblocking fd `0` poll/raw-read details for apps that want single-key
controls without carrying their own escape-sequence decoder. Console fd `0`
also wakes sleeping `poll()`/`select()` callers through the keyboard waiter, so
interactive tools that use POSIX-style readiness APIs do not need a separate
SmallOS input path. The same public
helper exposes `term_key_read_console()` for pager-style controls when fd `0`
is a pipe, plus `term_get_size()` for programs that need the current terminal
row/column geometry without including raw syscall headers.
Programs that redraw a whole screen, such as `top`, combine `term_keys.h` with
ANSI cursor/screen control written to fd `1`. That keeps live tools responsive
to single-key commands such as `q` without waiting for a newline.

PTY-backed GUI shells use the same descriptor contract. The GUI owns the PTY
master, the shell process inherits the slave on fd `0`/`1`/`2`, and child
programs launched by the shell inherit those descriptors unless marked
close-on-exec.

Graphics programs that need mouse motion can call `sys_mouse_read()` from
`src/user/internal/user_syscall.h`. It returns accumulated relative movement
and button bits from PS/2, VMware absolute-pointer translation, or the OHCI USB
boot mouse path, then clears the movement counters. This is a raw polling
helper, not a descriptor-backed event stream.

Framebuffer programs should use `gfx.h` rather than calling display syscalls
directly. `gfx.c` owns display acquire/release, XRGB8888 surface allocation,
rectangle copying, full-frame and rectangle presentation, temporary overlay
presentation through `gfx_present_surface()`, mapped framebuffer setup, and
indexed 8-bit palette presentation. Rectangle presentation uses strided blits
directly from the backbuffer; `gfx_context_t` no longer keeps a second
full-screen presentation surface. Its scratch surface is allocated lazily only
for operations that need packed or temporary pixels. `gfx_indexed.h` adds an
8-bit shadow framebuffer and dirty-rectangle tracking for Fractint-style
programs, while `gfx_text.h` provides bitmap text cells on top of a
`gfx_surface_t`.

Programs that already manage their own framebuffers can use `gfx_map()` after
acquiring the display. It wraps `SYS_DISPLAY_MAP`, records the user virtual
address and page layout for the page-flip framebuffer aperture, and lets
`gfx_present_mapped()` flip a rendered hidden page at a fresh vertical-retrace
edge. `gfx_present_indexed()` uses the mapped path when available and falls back
to a reusable XRGB scratch surface otherwise, so old 8-bit ports do not need to
carry private display syscall glue.

`sound.h` wraps the simple sound syscall surface. It provides PC speaker tone
helpers, bounded PIT pitch sequences, unsigned 8-bit PCM playback, AdLib/OPL2
register writes, kernel-timed OPL music sequences and single-voice OPL effects,
capability queries, status counters, and stop control. The kernel prefers AC97
PCI bus-master playback for PCM when present, falls back to Sound Blaster 16-bit
DMA when AC97 is absent, and exposes the 8-bit SB paths only where the caller
selects a diagnostic legacy path.

USB and mouse diagnostic commands use the same raw syscall layer instead of
running as kernel built-ins. `sys_usbinfo()`, `sys_mouse_debug()`,
`sys_usb_port_snapshot()`, `sys_usb_diag_op()`, and `sys_usb_mouse_op()` expose
snapshots or explicit diagnostic actions for `/bin/usb*` and
`/bin/mousetest`. `usbports` and the passive part of `usbdiag` format
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
driver's safety limit for the 32 MB test volume. The older whole-file
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
- `stat`, `lstat`, `fstat`, `statfs`, `fstatfs`, and `statvfs`
- `access`
- `openat`, `fstatat`, `unlinkat`, `mkdirat`, `renameat`, `linkat`,
  `symlinkat`, `readlinkat`, and `utimensat`
- `unlink`, `remove`
- `rename`
- `mkdir`, `rmdir`
- `fsync`, `ftruncate`, `fchmod`, `fchown`
- `getcwd`, `chdir`, `fchdir`
- `getpid`, `fork`, `execve`, `execv`, `execvp`, `waitpid`, `kill`,
  `killpg`, `setsid`, `getsid`, `setpgid`, `getpgid`, `getpgrp`, and
  `setpgrp`
- `getuid`, `geteuid`, `getgid`, `getegid`, `setuid`, `setgid`, and `umask`
- `getrlimit`, `setrlimit`, and `getrusage`
- `sysinfo`, `times`, `uname`, `ioctl`, `reboot`, signal-set helpers, and
  kernel-backed termios wrappers, including compatibility pieces used by
  BusyBox `getty` and `login`
- `popen` / `pclose` and `mntent` helpers for compatibility-oriented ports
- `system`, implemented through `shell -c command`
- `environ`, `getenv`, and `main(argc, argv, envp)` through `crt/crt0.c`
- `time`, `gettimeofday`, `clock_gettime`, `clock_settime`
- regex, fnmatch, basename/dirname, passwd/group, and getopt helpers
- socket, `sendto`, `recvfrom`, `sendmsg`, `recvmsg`,
  `getsockopt(SO_ERROR)`, network `ioctl`, `if_nametoindex`,
  `if_indextoname`, poll, epoll, timerfd, and signalfd wrappers used by guest
  services and BusyBox network applets
- `syslog`/`vsyslog`/`openlog` compatibility stubs for applets that optionally
  log through syslog on larger Unix systems

`access(path, mode)` now checks the caller's real uid/gid against the stored
mode bits reported by `stat`. Kernel syscalls enforce execute permission for
path traversal and exec, read/write permission for opens, write+execute on
parent directories for create/unlink/rename/link operations, and owner/root
rules for metadata mutation.

Directory-fd wrappers resolve relative paths against directory file
descriptors, reject non-directory base fds with `ENOTDIR`, and let absolute
paths override the supplied base fd. `fstatat` and `utimensat` honor
`AT_SYMLINK_NOFOLLOW`; `unlinkat` honors `AT_REMOVEDIR`.

The resolver accepts numeric IPv4 addresses and `localhost` without network
traffic, checks `/etc/hosts` when present, and can issue UDP DNS A-record
queries through the configured IPv4 DNS server. IPv6 remains unsupported and
returns the normal `EAI_FAMILY` path. The network `ioctl` wrapper routes
classic Linux-shaped `SIOC*` interface and route requests to kernel `eth0` and
loopback state for BusyBox `ifconfig` and `route`. Minimal `AF_NETLINK` route
sockets serve link, address, route, and ARP-neighbor state to BusyBox
`ip link`, `ip addr`, `ip route`, and `ip neigh`. `/proc/net/arp` exposes the
resolved ARP cache to BusyBox `arp`, and libc resolver/service shims cover
BusyBox `hostname`, `ipcalc`, `netstat -r`, `nslookup`, `pscan`, `whois`,
`ftpget`, `ftpput`, `tftp`, `tcpsvd`, `udpsvd`, `tftpd`, and `udhcpc`.

The runtime is also where SmallOS grows its hosted C surface. Older ports such
as Fractint are useful completeness tests: when they need a normal libc, libm,
or POSIX function, the preferred fix is to add that capability to the shared
runtime instead of hiding it in a per-program adapter. The source tree reflects
that boundary:

- `src/user/libc/` owns C library functions such as BSD string aliases, scanf
  parsing, stream helpers, `assert`, `rand`, `atof`, `atol`, and `system`
- `src/user/libm/` owns the freestanding math surface
- `src/user/posix/` owns POSIX APIs that are layered on kernel syscalls, such
  as `select()` on top of `SYS_POLL`
- `src/user/include/` owns the public libc/POSIX/SmallOS-extension headers,
  including compatibility names such as `<strings.h>`, `<malloc.h>`,
  `<endian.h>`, `<sys/dir.h>`, `<sys/file.h>`, `<dos.h>`, `<dir.h>`,
  `<conio.h>`, `<bios.h>`, `<fnmatch.h>`, `<libgen.h>`, `<pwd.h>`,
  `<grp.h>`, `<regex.h>`, `<termios.h>`, `<mntent.h>`, `<sys/vfs.h>`,
  `<sys/mount.h>`, `<sys/statvfs.h>`, `<sys/sysinfo.h>`, and `<sys/times.h>`
- `src/user/internal/` owns raw syscall and native runtime helper headers
- `src/user/posix/core.c` owns the broader syscall-backed descriptor and
  process wrappers
- `src/user/posix/dos_compat.c` and `src/user/posix/conio_compat.c` own the
  reusable DOS, Borland, BIOS, and console-keyboard compatibility entry points
- `src/user/libc/time.c` owns C time formatting/parsing helpers

SmallOS-specific helper headers keep recurring port pressure out of individual
adapters. `smallos_input.h` centralizes blocking keyboard waits,
`smallos_time.h` adapts the kernel tick counter to arbitrary guest tick rates,
`smallos_vga.h` provides planar VGA page math for DOS-era engines, and
`smallos_fs.h` exposes filesystem-capacity queries used by DOS disk helpers.

The Makefile archives those objects as `libc.a`, `libm.a`, and `libposix.a`,
then links user ELFs with `--gc-sections` so unused runtime functions do not
have to stay in each binary.

`stat`, `lstat`, and `fstat` use the full stat syscalls and fill inode number,
mode bits, link count, uid/gid, device id, size, block size, block count, and
ext2 timestamps. `stat` follows symbolic links, while `lstat` reports the link
inode itself. `chmod`, `chown`, `utimes`, `utimensat`, `fchmod`, `fchown`, and
`futimens` update ext2 metadata; `truncate` and `ftruncate` resize regular
files and zero-fill grown ranges. Newly created ext2 files, directories, and
special nodes apply the kernel-owned process `umask`, store the caller's
effective uid/gid, and enforce mode bits. Seeded regular files are executable
so the new exec permission checks can launch shipped programs.

`execve(path, argv, envp)` copies both argument and environment vectors into
kernel-owned storage before replacing the image. The copied launch context is
kept in PMM-backed per-process arenas, with room for 64 argv entries plus 1536
bytes of argv strings and 32 environment entries plus 1024 bytes of environment
strings. Passing `NULL` for `envp` inherits the caller's current environment.
`execv` and `execvp` use `environ`, and `execvp` searches `PATH`, falling back
to `/bin:/usr/bin:/usr/sbin`.

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

# Virtual Unix Compatibility Paths

The kernel recognizes a small virtual `/proc` and `/dev` surface through the
same open, stat, dirlist, and read paths used by ext2-backed files. These paths
exist for BusyBox and POSIX-shaped tools; they are read-mostly compatibility
interfaces, not complete Linux procfs or devfs implementations.
The ext2 image seeds `/proc`, `/dev`, `/dev/fd`, and `/dev/pts` as visible
mountpoint directories so `ls /` reflects the mounted virtual filesystems, but
the entries under those directories are still generated by the kernel.

`/proc` currently exposes system and process status:

- `/proc/meminfo`
- `/proc/uptime`
- `/proc/stat`
- `/proc/lastfault`
- `/proc/mounts`
- `/proc/filesystems`
- `/proc/net/arp`, `/proc/net/dev`, `/proc/net/raw`, `/proc/net/route`,
  `/proc/net/tcp`, `/proc/net/udp`, and `/proc/net/unix`
- `/proc/self`
- `/proc/<pid>/stat`
- `/proc/<pid>/status`
- `/proc/<pid>/cmdline`
- `/proc/<pid>/comm`

`/dev` currently exposes the common stream endpoints:

- `/dev/null`
- `/dev/zero`
- `/dev/urandom`
- `/dev/tty`
- `/dev/console`
- `/dev/fd/0`, `/dev/fd/1`, and `/dev/fd/2`
- `/dev/pts`

These nodes are intentionally narrow. They support the common reads, writes,
stats, and directory listings needed by tools such as `cat`, `ps`, `free`,
`df`, shell redirection, and compatibility probes. The terminal layer now
supports fd-backed `tcgetattr`, `tcsetattr`, `TIOCGWINSZ`, `TIOCSWINSZ`,
`TIOCSPGRP`, and `TIOCGPGRP` for console/PTY descriptors. `/dev/urandom` is
served by the kernel random generator and is seeded from the staged
`/etc/random-seed` image file plus runtime timing and platform entropy where
available. The mount table is kernel-backed for
the ext2 root, `/proc`, `/dev`, and dynamic `proc`/`devtmpfs` pseudo mounts;
`/proc/mounts`, `statfs`, `fstatfs`, `statvfs`, and `fstatvfs` report that
state. Foreground terminal stop/continue is backed by stopped process state,
`waitpid(..., WUNTRACED)`, process-group `SIGTSTP`/`SIGCONT`, and Ctrl+Z
handling for console and PTY foreground groups. Dynamic ext2 stacking, procfs
write knobs, authentication devices, and full Linux tty discipline semantics
remain out of scope for now.

---

# BusyBox Expectations

Native `/bin/login` is the normal boot gate. It reads `/etc/passwd` and
`/etc/shadow`, accepts the sample empty-shadow `root` account, applies uid/gid
and login environment variables, then starts the account shell (`/bin/shell` in
the default image). Native `/bin/passwd` sets `$smallos-sha256$` shadow hashes
and can clear root's password with `passwd -d root` for development recovery.

`usr/bin/busybox` is built from unmodified upstream source as the broad Unix
applet layer. The configuration keeps the native SmallOS command set intact,
while the SmallOS libc/sysroot provides the small POSIX/GNU compatibility
surface BusyBox expects: string helpers, unlocked stdio, `getline`, `%m`
formatting, terminal name wrappers, and a few POSIX process/filesystem shims.
BusyBox fills compatibility gaps with `ash` as `/bin/sh`, standalone applets,
core file tools, text filters, gzip/archive/hexdump tools, checksum helpers,
and lightweight process/filesystem diagnostics. Filesystem-facing applets such as `ln`,
`link`, `readlink`, `mkfifo`, `mknod`, `chmod`, `chown`, `chgrp`, `touch`,
`stat`, and `find -type` exercise the real ext2 metadata and node support.
Native `/bin` tools stay first in command lookup; if a bare command is missing,
the shell runs `/usr/bin/busybox <command> ...`. Interactive BusyBox `ash`
uses the ordinary console/PTY descriptor path, including termios state,
foreground process groups, and `poll()`/`select()` wakeups on stdin.

TinySSH is staged under `/usr/sbin` and started during boot through
`tinyssh-start`, which creates host keys on first run and execs BusyBox
`tcpsvd` on guest port `22`. Root public-key login uses `/.ssh/authorized_keys`;
interactive no-command sessions default to `/bin/shell`, and forced-PTY
sessions use the libc `openpty`/`login_tty` compatibility layer backed by the
kernel PTY syscalls. Service stdout and stderr are redirected to
`/var/log/tinyssh.log` inside the guest.

The current compatibility wave enables BusyBox `ash` job control, BusyBox
`mount`/`umount` for `proc`/`devtmpfs` pseudo mounts, classic networking tools
(`ifconfig`, `route`, and IPv4 `ping`), BusyBox `ip link`/`ip addr`/`ip route`/
`ip neigh`, ARP/hostname/calculation/inspection tools (`arp`, `hostname`,
`dnsdomainname`, `ipcalc`, `netstat -r`, `nslookup`, `pscan`, and `whois`), and
TCP/UDP applets (`nc`, plain HTTP `wget`, `ftpget`, `ftpput`, `tftp`,
`tcpsvd`, `udpsvd`, `tftpd`, `httpd`, and `udhcpc`), DHCP utility applets
(`udhcpd` and `dumpleases`), and login-flow applets (`init`, `login`, and
`getty`). Those applets are backed by kernel
stopped-job/process-group semantics, kernel network ioctls, minimal rtnetlink,
state-backed `/proc/net/dev`, `/proc/net/route`, and `/proc/net/arp`,
ARP-neighbor reporting, raw ICMP sockets, UDP DNS/TFTP traffic, UDP
`SO_REUSEADDR`/`MSG_PEEK` service compatibility, native `SYS_NET_OP_DHCP` for
BusyBox `udhcpc`, credential syscalls, `/etc/passwd`/`/etc/shadow`/`/etc/group`,
and terminal/session compatibility wrappers for BusyBox `init`/`login`/`getty`.
Arbitrary filesystem mounting, real BusyBox PID-1 boot supervision,
Linux-compatible password hash semantics, BusyBox DHCP server operation and
lease renewal/expiry semantics, telnet/telnetd, inetd, richer TFTP server
modes, AF_PACKET applets, `ip` rule/tunnel modes, multi-interface and IPv6
behavior, HTTPS/TLS, and complete Linux device behavior remain disabled or
stubbed. New BusyBox applets should grow the shared runtime, headers, and
virtual filesystem behavior when they reveal a portable Unix expectation,
rather than carrying local source patches against BusyBox itself.

---

# TinyCC Expectations

`usr/bin/tcc` is built directly from the TinyCC submodule plus the SmallOS user
libraries; SmallOS does not patch TinyCC for the guest compiler path. It links
`src/user/crt/crt0.c`, so the kernel still enters `_start(argc, argv)` while
TinyCC itself runs through its upstream `main(argc, argv)` path. Inside the
guest it searches `/usr/include` and `/usr/lib`, adds conventional startup and
support files such as `crt1.o`, `crti.o`, `crtn.o`, `libtcc1.a`, and
`runmain.o`, and can compile normal hosted `main()` programs without
`-nostdlib`. TinyCC's upstream `-bt` runtime is available through `bt-exe.o`,
`bt-log.o`, signal/ucontext delivery, and a conventional `libpthread.a`
placeholder archive. The v1 target still inherits TinyCC's upstream i386
`__linux__` predefine; SmallOS treats that as a documented compatibility wart
until a separate upstreamable target identity exists. It relies on normal
runtime behavior:

- cwd-aware file opens
- normalized path handling through `realpath`
- fd-backed stdio streams
- meaningful `fflush`
- directory traversal through `opendir` / `readdir`
- ext2 writeback through normal streaming fd writes and flush/close when the mounted storage is writable
- `SA_SIGINFO` signal actions with i386 `ucontext_t` register state for `tcc -bt -run` crash reporting

The TinyCC acceptance gate is the guest compiler suite:

```text
tinycc_math
tinycc_agg
tinycc_tree
tinycc_compile
tinycc_sysroot
tinycc_posix
tinycc_run
```

Any runtime or filesystem change should keep those tests passing unless the
change explicitly updates the TinyCC contract and tests in the same patch.

---

# Binutils Expectations

GNU binutils is built from upstream source as hosted SmallOS userland and
installed under `/usr/bin` as `addr2line`, `ar`, `as`, `ld`, `nm`, `objcopy`,
`objdump`, `ranlib`, `readelf`, `size`, `strings`, and `strip`. SmallOS only
claims the staged tool surface, not the full upstream binutils test matrix.
The build intentionally leaves out `gold`, `gprof`, `gprofng`, plugins, NLS,
and shared binutils libraries.

The runtime contract is the generated-ELF path covered by `binutilsprobe`:
temporary files under `/tmp`, fd-backed stdio, correct `F_GETFL` access mode
bits, archive parsing through `scanf`/`fscanf` `ll` integer formats, normal
`fork`/`execve`/`waitpid`, ext2 writeback for object/archive/ELF outputs, and
execution of static i386 ELFs linked by the guest `ld`.

The acceptance gate is:

```text
shell: binutils_smoke
elf: binutilsprobe
```

`binutils_smoke` keeps the command surface visible from the shell.
`binutilsprobe` assembles a tiny program, creates an archive without a symbol
index, indexes it with `ranlib`, links a generated executable, checks the ELF
with the inspection tools, copies and strips it, and verifies that the linked,
copied, and stripped ELFs all print `binutils roundtrip ok`.

The generic CRT adapter is also the preferred startup path for new hosted-ish
SmallOS user programs:

```c
int main(int argc, char** argv);
```

The kernel launches `void _start(int argc, char** argv, char** envp)`.
`crt0` provides that symbol, sets global `environ`, calls
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
- `permprobe` - kernel-owned credentials, `umask`, mode-bit enforcement, and
  non-root access denial
- `atprobe` - directory-fd `*at()` syscalls, absolute path override,
  non-directory base errors, and no-follow symlink stat/timestamp behavior
- `ttyprobe` - kernel-backed termios state, PTY echo/raw/EOF behavior,
  `openpty`/`ttyname`/`ptsname`/`login_tty`, terminal size/process-group
  ioctls, and non-terminal `ENOTTY` failures
- `rsrcprobe` - resource limits, `RLIMIT_NOFILE` enforcement, and
  `getrusage` self/children accounting behavior
- `sessprobe` - session ids, process groups, terminal foreground pgrp wrappers,
  child inheritance, and invalid process/session failures
- `mountprobe` - state-backed mount listings, per-mount `statfs`/`fstatfs`,
  dynamic `proc`/`devtmpfs` pseudo mounts, gated ext2 stacking, and busy
  unmount failures
- `netbbprobe` - BusyBox-facing `eth0` and loopback interface/route ioctls,
  minimal rtnetlink link/address/route/neighbor dumps, `/proc/net/dev`,
  `/proc/net/route`, UDP datagrams, and localhost/numeric resolver behavior
- `stdioprobe` - EOF/error state, `clearerr`, `fflush`, invalid stdio ops
- `dirprobe` - root and nested directory iteration, EOF, invalid/missing dirs
- `errnoprobe` - wrapper `errno` behavior
- `compatprobe` - BusyBox-facing `statfs`, `/proc`, `/dev`, `/dev/urandom`,
  `fchdir`, `/bin/sh`, metadata mutation, links/symlinks, special nodes,
  truncate, and compatibility wrappers
- `crtprobe` - `main(argc, argv)` via `crt0`, argv terminator, and return status
- `waitprobe` - `SYS_EXEC` pid return, `waitpid`, `WNOHANG`, `kill`, and wait status macros
- `jobctlprobe` - stopped child reporting, `WUNTRACED`, process-group `SIGTSTP`/`SIGCONT`, and `killpg`
- `pipeprobe` - pipe read/write, EOF, `EPIPE`, nonblocking behavior, `PIPE_BUF`, poll readiness, and blocking transfer wakeups
- `dupprobe` - `dup`, `dup2`, shared file offsets/status flags, and independent `FD_CLOEXEC`
- `forkprobe` - parent/child return values, copied memory independence, `waitpid`, and inherited shared file offsets
- `cowprobe` - copy-on-write fork isolation for globals, heap,
  mmap-anonymous pages, stack writes, lazy frame allocation, partial
  `munmap`/fixed remap behavior, `mprotect` write restoration across fork, and
  shared-frame accounting rise/fall
- `memleakprobe` - PMM/kalloc drift across fork, pipe, file, mmap, and socket churn
- `procscaleprobe` - more-than-32 child process scaling, PID uniqueness, and accounting release
- `lifetimeprobe` - composed process/exec, fd, pipe, file, timerfd/signalfd,
  mmap/COW, and `sys_meminfo` accounting cleanup across repeated cycles
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
