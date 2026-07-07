# Execution Model

This document describes how commands are dispatched, how ELF programs are loaded, and how the current scheduler / syscall model actually behaves.

For the C runtime contract exposed to those ELF programs, including cwd,
`errno`, fd wrappers, stdio, and directory APIs, see
[`docs/user-runtime.md`](user-runtime.md).

It reflects the current code in:

- `src/exec/elf_loader.c`
- `src/kernel/process.c`
- `src/kernel/scheduler.c`
- `src/kernel/syscall.c`

---

# Overview

ELF programs are scheduler-owned user processes. The boot path launches the
user shell from `/bin/shell`, and the user shell uses the kernel loader
machinery for child programs:

```text
user shell command
  → <name> [args]
  → vfs_load_file_owned()
  → elf_run_image()
  → sched_enqueue(proc)
  → process_wait(proc)
  → scheduler enters child via elf_user_task_bootstrap()
  → ring-3 entry via iret
  → syscalls via int 0x80
  → sys_exit() → sched_exit_current()
  → child becomes ZOMBIE, waiter destroys it

user shell command or kernel fallback command
  → bg <name> [args]
  → vfs_load_file_owned()
  → elf_run_image()
  → sched_enqueue(proc)
  → return immediately

user shell command
  → bg <name> [args]
  → vfs_load_file_owned()
  → elf_run_image()
  → sched_enqueue(proc)
  → process_claim_for_wait(proc)
  → store proc in the shell job table
  → return immediately
```

Important current-state facts:

- the default foreground boot program is native `/bin/login`, a scheduler-owned
  ring-3 user process launched by `bootseq`; successful login starts
  `/bin/shell`, and `bootseq` has an emergency shell fallback if login cannot
  load
- **keyboard IRQ1** decodes scancodes and calls a registered `keyboard_consumer_fn` — it makes no routing decisions itself; USB boot keyboards inject translated set-1 scancodes into the same path
- the **process consumer** (`process_key_consumer` in `process.c`) pushes ASCII into `kb_buf`; Ctrl+C is delivered as raw byte `0x03` to a foreground `SYS_READ_RAW` prompt reader, and otherwise targets the foreground process group as a terminal signal
- Ctrl+Z either detaches a native shell-owned foreground job back to the shell
  job table or sends `SIGTSTP` to the foreground process group so POSIX-style
  shells can observe a stopped job with `waitpid(..., WUNTRACED)`
- consumer ownership transfers via `process_set_foreground()` - the process consumer is active while the user shell or another user process owns the foreground reader/group; `process_set_foreground(0)` keeps the router installed but ignores events until a new foreground owner is set
- **Mouse input** decodes PS/2 relative packets, VMware absolute-pointer
  events, or OHCI USB boot mouse reports into accumulated `dx`/`dy` and button
  state. Older graphics demos can poll that state with `SYS_MOUSE_READ`; mixed
  keyboard/mouse event loops use `SYS_INPUT_READ`, and can sleep until input or
  an absolute frame deadline with `SYS_INPUT_WAIT_UNTIL`.
- **ELF user programs** are loaded into their own page directory and do execute in ring 3
- dynamic user ELFs with `PT_INTERP=/lib/ld-smallos.so` are supported; the
  kernel loads both the main image and interpreter, then the user-space loader
  maps `/lib/libc.so` through `mmap`
- ELF launch and exit are now scheduler-owned: `elf_run_image()` seeds a bootstrap context, enqueues the task, and returns `process_t*`
- the scheduler supports kernel tasks, ELF tasks, voluntary yielding, timer-driven sleeping, and timer-driven switching; foreground commands wait for children, and `bg` returns while keeping a reattachable shell job
- user ELFs have a small freestanding runtime layer with a heap allocator,
  fd-backed console streams, streaming VFS-backed file handles,
  `stat`/`rename`/`unlink`, `lseek`, and socket wrappers, which is enough for
  compiler-style tools and small network services
- GUI shell windows allocate a PTY pair, fork a user shell with the slave on fd
  `0`/`1`/`2`, and keep the master in the GUI process so foreground commands
  draw inside the window instead of the global console. The GUI paces frame
  work and shell PTY polling so quiet windows do not keep the desktop awake.
- the shipped `usr/bin/tcc` compiler binary links the generic SmallOS `crt0` adapter and runs TinyCC's normal `main`, can compile guest C sources from ext2, write the results back to disk, and then those generated ELFs can be executed immediately
- the shipped `usr/bin/busybox` binary provides the broader Unix applet layer;
  native SmallOS commands remain first in shell lookup, `/bin/sh` launches
  BusyBox `ash` with basic job control, and missing bare commands fall back to
  `/usr/bin/busybox <command> ...`
- QEMU user networking is still the default for `make run` / `make test`, but the guest now learns its IPv4 address, netmask, gateway, DNS server, and lease time through DHCP instead of assuming QEMU's NAT addresses. `make run-tap` switches the NIC onto a host TAP device for bridged or routed networking beyond QEMU's built-in NAT.
- Boot queues DHCP and best-effort NTP as an async kernel task once the scheduler is live. On success, `CLOCK_REALTIME` is set and the boot log prints the UTC time; on failure, boot continues with a warning.
- Protected-mode boot diagnostics are muted on the active display, mirrored to
  serial, and saved to `/var/log/boot.txt` with `[ms=... tick=... cyc=...]`
  prefixes. DHCP, NTP, and default-service messages produced while the splash
  is visible are display-suppressed but still appended to the boot log.
- `pinggw` and bare `ping` target the DHCP-provided gateway. `ping <ip>` routes through the DHCP gateway when the target is off-subnet. Public ICMP may still be blocked by the surrounding hypervisor or NAT, so `pingpublic` is only a best-effort probe.
- `netcheck` prints the gateway steps separately from the public ICMP probe so a `1.1.1.1` timeout does not imply the local NAT path is broken
- `usr/sbin/tcpecho`, `usr/sbin/sockeof`, `usr/sbin/ftpd`, and `usr/sbin/cserve` are the current guest-side TCP smoke apps; they run as normal ELFs and are exercised through QEMU hostfwd on the guest service ports
- `tcpecho` listens on `2323` in the guest and is driven by `make socket-parallel-smoke` to verify multiple simultaneous echo clients
- `sockeof` listens on `2463` in the guest and is driven by `make socket-eof-smoke` to verify a multi-segment payload before EOF, `POLLHUP`, post-EOF response writes, and guest write-side shutdown
- `ftpd` listens on `2121` in the guest and expects passive data connections on `30000`; the boot sequence starts it in quiet mode, `make ftp-smoke` and `make ftp-loop-smoke` cover that path, and host-side clients such as `lftp`, WinSCP, and FileZilla should use passive mode
- `cserve` listens on `8080` from `/var/www` by default; the boot sequence starts it with logging disabled and `max-conn` set to 28

---

# Kernel Fallback Command Flow

```text
keyboard IRQ1
  ↓
keyboard_handle_irq()
  ↓
decode scancode → key_event_t
  ↓
call s_consumer(ev)   ← registered keyboard_consumer_fn
  ↓
process_key_consumer()
  ↓
ASCII enters kb_buf for SYS_READ; Ctrl+C targets foreground process group
```

Mouse input is separate from the foreground keyboard consumer path:

```text
Mouse IRQ12 / OHCI USB boot mouse poll
  ↓
mouse_handle_irq() or mouse_inject_relative()
  ↓
decode PS/2 packet, VMware event, or USB boot report → accumulate dx/dy/buttons
  ↓
SYS_MOUSE_READ copies state to userland and clears dx/dy
```

After kernel diagnostics, `kernel_main()` creates `bootnet`, `bootsvc`, and
`bootseq` kernel tasks and enters the scheduler on `bootseq`. `bootseq` mounts
ext2, saves `/var/log/boot.txt`, switches async chatter to log-only mode, loads
`/bin/login` suspended, and runs `/bin/bootsplash boot/splash.bmp`.
While the splash remains visible, boot input/HID diagnostics finish and
`bootnet`/`bootsvc` append DHCP, NTP, FTP, and cserve status to the boot log.
`bootseq` then clears the splash, prints a welcome/time/network/memory summary
plus `SmallOS ready`, and launches `/bin/login`. The native login prompt checks
`/etc/passwd` and `/etc/shadow`; the sample root shadow password starts empty
and can be changed with `/bin/passwd`. Successful login starts `/bin/shell`;
when the shell exits, `bootseq` launches login again. If `/bin/login` cannot be
loaded, `bootseq` falls back to an emergency `/bin/shell` when available.

---

# Supported Program Paths

## 1. Shell commands and app commands

The user shell resolves bare command names through `/bin/<name>`,
`/usr/bin/<name>`, and `/usr/sbin/<name>`. Path-like command names are resolved
relative to the shell cwd. Commands like `echo`, `about`,
`uptime`, `halt`, `reboot`, `date`, `pwd`, `cat`, `fsread`, `ls`, `tree`,
`touch`, `rm`, `mkdir`, `rmdir`, `cp`, `mv`, `edit`, `meminfo`, `memmap`,
`cpuz`, `top`, `netinfo`, `ping`, `dhcp`, `ataread`, `usbinfo`, `usbports`,
`usbdiag`, `usbpeek`, `usbpower`, `usbmouse`, `mousetest`, `ip`, and
`ipconfig` are shipped this way under `/bin/`.

Larger demo and port binaries are staged under `/usr/bin/`, including
`hello`, `plasma`, `mandel`, `fractint`, `wolf3d`, `tcc`, and `busybox`.
Regression probes that are useful to keep out of the normal command namespace
live under `/usr/libexec/tests/`, including `mathprobe`, `compatprobe`, and
`wolf3d-srcprobe`.

If no native command matches a bare name, the shell tries
`/usr/bin/busybox <name> ...`. That keeps native SmallOS behavior preferred
while making applets such as `grep`, `sed`, `awk`, `df`, `du`, `free`, `ps`,
`tar`, and `hexdump` available as ordinary commands. BusyBox `ash` also uses
the kernel process-group stop/continue path for `jobs`, `fg`, `bg`, and Ctrl+Z.

The interactive shell editor keeps a short command history and command/path
completion. History stores the full input line before tokenization, so recalled
entries retain all arguments even when the command failed. Completion merges
duplicate visible candidates, such as a built-in command that also exists as a
program under `/bin`, before deciding whether there is a unique match.

`/bin/ip` and `/bin/ipconfig` are the user-facing runtime network
configuration tools. They read NIC/IPv4/TCP state through `SYS_NETINFO` and
update the runtime IPv4 address, route, DNS, or DHCP lease through `SYS_NET_OP`.
Those settings are intentionally volatile; rebooting returns to boot-time DHCP.

When the shell launches an app command, the child inherits the current shell
cwd and standard descriptors before it becomes runnable. That preserves
relative path behavior and lets GUI-launched children write back through the
same PTY-backed shell window.

`edit` is a normal foreground ELF, not a kernel shell mode. It uses raw
console reads and ANSI-style cursor control to run as a full-screen text
editor, so Ctrl+C/Ctrl+Z foreground job control still belongs to the same
process-management path as other interactive ELFs.

## 2. Program execution

Typing an external command is the normal user-program path. The shell resolves
the command to a filesystem path, calls the foreground exec syscall, and waits
for the child unless the command was launched through `bg`.

That means `argv[0]` inside the process is the command token as typed. For
`hello alpha beta`, `argv[0]` is `hello`; for
`usr/bin/hello alpha beta`, `argv[0]` is `usr/bin/hello`.

There is **no active `runimg` command path** in the current shell command table.
The shell also supports `shell -c "command args"` for non-interactive command
execution; libc `system()` uses that path. `/bin/login` is the normal boot gate
and starts the native shell after authentication. `/bin/sh` is separate: it
launches BusyBox `ash` for script-style POSIX compatibility, including basic
stopped job control, without replacing the native interactive `/bin/shell`.

The same path is used by the guest TinyCC smoke tests:

```text
tcc -nostdlib -o var/tmp/tccmath usr/share/examples/tinycc/tccmath.c
var/tmp/tccmath
tcc -o var/tmp/tccsysroot usr/share/examples/tinycc/tccsysroot.c
var/tmp/tccsysroot
tcc -o var/tmp/tccposix usr/share/examples/tinycc/tccposix.c
var/tmp/tccposix
```

The test suite uses this flow to compile several focused C samples inside the
guest and then run the generated programs. The `-nostdlib` samples exercise
freestanding output, while `tccsysroot.c` exercises the installed
`/usr/include` and `/usr/lib` hosted path. `tccposix.c` goes further and uses
that sysroot for ordinary file, stat, cwd, directory, stderr, time, header
compatibility, and `system()` APIs. The sample sources are staged under
`/usr/share/examples/tinycc/` and the produced binaries are written under
`/var/tmp/`.

TinyCC's runtime expectations are part of the user runtime contract in
[`docs/user-runtime.md`](user-runtime.md).

For the TCP service path, FTP and cserve are boot-started by default. The shell
can still launch additional long-lived reattachable services with commands such
as `bg usr/sbin/tcpecho`, `bg usr/sbin/sockeof`, or
`bg usr/sbin/ftpd --log-file /var/log/ftpd.log`. Those programs bind and listen
inside the guest, and you connect to them from the host through QEMU `hostfwd`.
Use `jobs` to inspect them, `fg <jobid>` to wait on one in the foreground,
Ctrl+Z to return a foregrounded job to the background, and `kill <jobid>` to
stop one without rebooting the guest. Boot-started `ftpd` runs in quiet mode so
request logs do not interrupt the shell prompt.

The FTP service uses passive data connections, so a host-driven smoke needs
both the control port and passive data port forwarded:

```text
hostfwd=tcp::2121-:2121,hostfwd=tcp::30000-:30000
```

`make ftp-smoke` sets those forwards, uses the boot-started `ftpd`, and verifies
login, negative path replies, directory listing, download, upload readback,
delete, and `RMD` cleanup. If the shell prompt appears before the async boot
DHCP task has acquired a lease, the harness runs the guest `dhcp` command
before opening the host-forwarded FTP connection.

`make ftp-loop-smoke` uses the same forwards and the same boot-started `ftpd`,
then repeats fresh control sessions with passive `LIST`, `RETR`, `STOR`,
uploaded-file readback, and cleanup cycles.

`make socket-parallel-smoke` forwards guest port `2323`, launches `tcpecho`,
opens 8 parallel echo clients by default, verifies small payload responses on
each client, and records `netinfo` before, during, and after the run.

`make socket-eof-smoke` forwards guest port `2463`, launches `sockeof`, sends a
3072-byte patterned payload followed by a host TCP half-close, and verifies that
the guest drains the complete payload, observes EOF through `poll()`/`read()`,
can still send a final response, and then uses `shutdown(SHUT_WR)` to reject
later writes and deliver EOF to the host. It then opens a second connection
where a final guest write is delivered before guest `close()` sends FIN and the
host observes EOF.

`make cserve-smoke` forwards guest port `8080`, uses the boot-started cserve
instance, checks the large `/var/www/index.html` static fixture, holds
24 keep-alive clients by default, exercises a slow reader, verifies
`/favicon.ico` returns 404, and records the guest `netinfo` socket/TCP summary.

`make busybox-net-smoke` checks BusyBox `udhcpc` through the native SmallOS
DHCP operation, forwards guest BusyBox `httpd`, `tcpsvd`, and `udpsvd`/`tftpd`
ports, fetches `/var/www` content from the host, serves tiny host
HTTP/FTP/TFTP/WHOIS/echo fixtures for guest BusyBox clients, checks BusyBox
`nc` against a host echo socket, starts BusyBox `tcpsvd` in the guest for a
host-forwarded echo check, and fetches a file from guest BusyBox `tftpd`
through `udpsvd`. This keeps the enabled BusyBox network applets tied to real
IPv4 TCP/UDP behavior rather than only compile-time availability. It
deliberately does not start BusyBox `udhcpd`.

---

# ELF Load Path

## Name lookup and file read

`elf_run_named(name, argc, argv)` does:

```text
vfs_load_file_owned(name, &size, &frame, &frames)
  → backend file lookup
  → follow block chain through the selected ext2 storage source
  → copy file into a private PMM-backed temporary image buffer
  → return pointer plus the backing frame range
```

Important invariant:

- `ext2_load()` still returns a pointer into a shared internal buffer
- named ELF loading uses `vfs_load_file_owned()` so a preempted large image load
  cannot be overwritten by another filesystem load before segment mapping

`elf_run_named()` then passes that image pointer to `elf_run_image()`.

## ELF validation and process creation

`elf_run_image()`:

- validates ELF magic
- allocates a fresh `process_t` with `process_create("elf")`
- allocates a fresh page directory with `process_pd_create()`
- maps each `PT_LOAD` segment into user memory using PMM frames
- copies file-backed bytes from the ext2 load buffer into those frames
- allocates and maps one user stack page
- allocates one kernel stack frame for privilege transitions

## Address-space layout

Current user process setup:

- ELF segments are mapped at their link/load virtual addresses
- user stack is mapped at `USER_STACK_TOP - USER_STACK_SIZE`
- kernel mappings remain present in the process page directory through copied kernel PD entries
- ring-3 code can only access pages marked `PAGE_USER`

The process page directory is therefore a **hybrid** layout:

- private user region for the process
- shared kernel mappings for syscall / interrupt entry and kernel code

---

# Ring-3 Entry

After mappings are created, `elf_run_image()` prepares the task for its first scheduled entry.

The code currently does all of the following:

1. allocate the process kernel stack
2. seed `proc->sched_esp` so the first scheduled kernel context returns into `elf_user_task_bootstrap()`
3. mark the process `PROCESS_STATE_RUNNING`
4. enqueue it with `sched_enqueue(proc)`
5. return the `process_t*` to the caller

`tss_set_kernel_stack()` is **not** called during `elf_run_image()` setup. That update happens later inside `elf_user_task_bootstrap()`, at the moment the scheduler first enters the new process. This avoids clobbering the currently running task's ESP0 during async launch paths such as `bg` and `SYS_EXEC`.

For foreground commands, the shell then waits until the child reaches
`PROCESS_STATE_ZOMBIE`. For `bg`, the shell claims the child and stores its pid
in a small job table so the process can be listed, foregrounded, or killed
later.

The user shell keeps its larger scripted regression command lists in static
storage. That matters for `shelltest` and `selftest`, which run through the same
foreground shell context while launching and waiting on many child tasks.

`elf_enter_ring3()` then:

- copies argv strings onto the user stack
- builds the `argv[]` pointer array on that same user stack
- pushes a fake return address, `argc`, and `argv`
- loads user data segments
- pushes an `iret` frame
- sets IF in the pushed EFLAGS
- executes `iret`

That transitions the CPU from CPL 0 to CPL 3 and begins execution at `e_entry` with interrupts enabled.

---

# Parent / Child Tracking

The current design combines scheduler ownership, foreground input ownership, a
small process registry, and automatic zombie reaping:

- foreground shell execution launches the child, then waits for it
- `bg` launches the child and returns immediately, but shell job control owns cleanup until `fg <jobid>` reaps it or `kill <jobid>` stops it
- Ctrl+Z while a shell-owned job is foregrounded detaches it back to the shell job table without killing it
- POSIX-style shells such as BusyBox `ash` rely on the kernel stopped state:
  terminal Ctrl+Z sends `SIGTSTP` to the foreground process group,
  `waitpid(..., WUNTRACED)` reports the stopped child once, and `SIGCONT`
  resumes it
- `SYS_EXEC` returns the child pid and claims the child for its parent so userland can call `waitpid()`
- if a parent exits without waiting, its children are orphaned and any unclaimed zombies become reaper-owned
- interactive foreground input is tracked with `process_set_foreground(proc)` / `process_get_foreground()`, while terminal signals target `process_get_foreground_group()`
- Ctrl+C is delivered to the current foreground process group as a terminal interrupt during normal `SYS_READ` input. Matching signalfds receive `SIGINT`; otherwise group members exit with status `130` and the waiting shell path is restored. A foreground process waiting in `SYS_READ_RAW`, including the shell prompt editor, receives byte `0x03` instead so it can handle line cancellation itself.
- process destruction is explicit via `process_wait()` / `waitpid()` or automatic via `sched_reap_zombies()`
- POSIX-shaped process replacement is available through `fork()` + `dup2()` +
  `execve()` / `execvp()`; the legacy `SYS_EXEC` spawn path remains supported.

---

# SYS_EXEC, Fork, And Execve

`sys_exec_impl()` in `src/kernel/syscall.c` is **spawn-style**:

- copy program name into a kernel buffer
- call `elf_run_named()`
- claim the child for parent-side waiting
- return the child pid on success, or a negative errno such as `-ENOENT` / `-EFAULT` on failure

`elf_run_named()` follows the same scheduler-owned ELF launch path as shell commands: create the process, seed its bootstrap context, enqueue it, and return immediately.

`SYS_FORK` clones the current user process with eager address-space copying and
duplicates the fd table as shared descriptor entries. The parent receives the
child pid, while the child resumes from the same syscall frame with return value
`0`. `SYS_EXECVE` then replaces the current user image in-place, preserving pid,
cwd, process group, and descriptors that do not have `FD_CLOEXEC` set.

The file, console, and socket syscalls used by shell tools, TinyCC, and the
FTP/TCP smoke apps now share the dynamic PMM-backed process handle table owned
by `process.c`. Each handle has readable/writable/dirty state plus ops for
`read`, `write`, `seek`, `poll`, `flush`, and `close`; file, pipe, and socket
descriptors are shared/refcounted where POSIX expects duplicated descriptors or
fork-inherited descriptors to see the same underlying state. Socket handles
point at kernel `socket_t` objects whose accept/read/write wait queues wake
blocking socket syscalls and socket-backed poll/epoll waits. Timerfd/signalfd-style
handles own read wait queues too, with expired timerfds woken from the timer IRQ
path. Accepted TCP streams now live in a global 4-tuple TCP table, allocate a
lazy 4 KiB PMM-backed RX ring on
first payload, and advertise the remaining receive window; socket writes use a
lazy 16 KiB TX ring, ACK-driven space reclamation, release-on-drain behavior,
and write-waiter wakeups for basic send-side backpressure.
`shutdown()` half-close behavior is implemented for passive and active close
paths: `SHUT_RD` reports local EOF, and `SHUT_WR` drains queued TX before
sending FIN and rejecting later writes. FIN paths retransmit, ACK duplicate peer
FINs, preserve final writes before close-driven FIN, and clean up once the peer
close/ACK sequence completes or idles out.
Each process also carries cwd state, so user path syscalls resolve relative
paths before entering VFS or ELF loading.
ext2-backed file behavior and path operations sit behind `vfs.c`, so
`syscall.c` stays focused on validation and dispatch instead of handle
lifetime or resource-specific behavior.

---

# Exit Path

A user ELF exits through:

```text
sys_exit()
  ↓
int 0x80
  ↓
syscall_handler_main()
  ↓
sys_exit_impl()
  ↓
sched_exit_current((unsigned int)regs)
```

`sys_exit_impl()` currently does this:

1. switch CR3 to the kernel page directory
2. call `sched_exit_current((unsigned int)regs)`
3. mark the current task `PROCESS_STATE_ZOMBIE`
4. dequeue it from the run queue
5. switch to the next runnable task

Important invariant:

- `sched_exit_current()` must **not** destroy the task immediately because the kernel is still running on that task's kernel stack
- destruction is deferred until a waiter such as `process_wait()` observes `PROCESS_STATE_ZOMBIE` and calls `process_destroy()` from a safe stack

---

# Scheduler Interaction

The scheduler is real, preemptive, and fully owns ELF launch.

## Timer path

```text
irq0_stub
  ↓
irq0_handler_main(esp)
  ↓
timer_handle_irq()
  ↓
PIC EOI
  ↓
sched_tick(esp)
```

## Boot path

```text
kernel_main()
  terminal_init()
  gdt_init()
  paging_init()
  memory_init()
  pmm_init()
  keyboard_init()
  mouse_init()
  timer_init(SMALLOS_TIMER_HZ)
  idt_init()
  sched_init()
  ata_init()
  mount ext2 from ATA, USB storage, or loader2 boot RAM fallback
  create bootseq kernel task
  sched_enqueue(boot_proc)
  process_start_reaper()    ← creates and enqueues reaper task
  sched_start(boot_proc)    ← IF remains masked for the first stack switch
  kernel task bootstrap enables IF
  bootseq mounts ext2, saves boot log, preloads user shell suspended
  bootseq runs startup splash while async network/services log quietly
  bootseq clears display, foregrounds and resumes user shell
```

## What the scheduler owns

The scheduler owns everything:

- kernel tasks created with `process_create_kernel_task()` (shell, reaper)
- ELF user tasks — `elf_loader.c` copies argv into `process_t` storage, builds a valid `sched_esp` on the process kernel stack, seeds first scheduler entry through `elf_user_task_bootstrap()`, and enqueues the process with `sched_enqueue(proc)`
- voluntary yields via `SYS_YIELD` → `sched_yield_now()`
- timer-driven preemption via `sched_tick()`
- exit via `SYS_EXIT` → `sched_exit_current()`

The active execution path is:

- **ELF launch uses `sched_enqueue(proc)`**
- **foreground commands wait with `process_wait()`**
- **`bg` children are reaped automatically by the reaper task**
- **`SYS_EXEC` children are collected by `waitpid()` or orphaned to the reaper when the parent exits**
- **user-shell `bg` children are claimed by shell job control so they can be listed, foregrounded, or killed**

---

# Process Ownership Rules

There are currently two related ownership concepts.

`process_get_current()` follows the scheduler-owned current task. Interactive input routing uses foreground ownership via `process_set_foreground()` / `process_get_foreground()`, with terminal signals scoped to the foreground process group.

That means:

- during normal shell execution, the current process is the scheduler-owned shell task
- while the shell is waiting on a foreground ELF, keyboard routing still follows the foreground reader first
- otherwise keyboard routing falls back to `sched_current()`

---

# Ring-3 Privilege Model

ELF processes run at CPL 3.

Properties of the current setup:

- user pages are mapped with `PAGE_USER`
- kernel pages are present but not user-accessible unless explicitly marked user
- `int 0x80` is callable from ring 3
- on syscall / interrupt entry from ring 3, the CPU switches to the kernel stack pointed to by the TSS
- `tss_set_kernel_stack()` must always match the active ring-3 process before ring-3 entry resumes

---

# Argument Passing

When `usr/bin/hello a b` is invoked, the ELF sees:

- `argc = 3`
- `argv[0] = "usr/bin/hello"`
- `argv[1] = "a"`
- `argv[2] = "b"`

The user stack is constructed manually by `elf_enter_ring3()`.

High-level layout just before `iret`:

```text
[string data]
[4-byte alignment]
argv pointer array
fake return address = 0
argc
argv
```

The entry point receives a normal C-style `(int argc, char** argv)` call frame.

This is the launch contract for every user ELF:

- the kernel enters the ELF symbol selected by `-e`, normally `_start`
- the low-level entry ABI is `void _start(int argc, char** argv, char** envp)`
- `argv[argc]` is `NULL`
- `envp` is NULL-terminated
- `argv` / `envp` strings and pointer arrays live on the initial user stack
- `process_set_args()` and `process_set_env()` own the process-side copies before ring-3 entry
- returning from `_start` is unsupported unless a CRT layer converts the return value into `sys_exit`

`src/user/crt/crt0.c` is that CRT layer for hosted-ish programs. It keeps the
kernel ABI at `_start(argc, argv, envp)`, sets `environ`, calls
`main(argc, argv, envp)`, and exits with the returned status. TinyCC is linked
this way so its upstream `main` path can run normally. The seed image installs
`/usr/lib/crt0.o` so guest-built hosted programs can use the same entry path.
Direct `_start(argc, argv)` programs remain supported for low-level probes and
freestanding tests; new hosted-ish programs should prefer
`int main(int argc, char** argv, char** envp)` plus `crt0`.

## Dynamic Executable Handoff

When an executable has no `PT_INTERP`, the kernel follows the static path and
enters the program directly. Non-interpreted `ET_DYN` images are rejected for
now. When `PT_INTERP` names `/lib/ld-smallos.so`, the kernel maps a legacy
`ET_EXEC` main executable at its fixed address or a PIE `ET_DYN` main
executable at deterministic `USER_PIE_BASE`, maps the interpreter, builds the
normal argv/envp stack plus a small auxv, and enters the interpreter instead.

The auxv contract currently includes:

```text
AT_ENTRY  main executable entry point
AT_PHDR   main executable program-header address
AT_PHENT  program-header entry size
AT_PHNUM  program-header count
AT_BASE   interpreter load base
AT_PAGESZ user page size
```

`ld-smallos.so` is a base-zero `ET_DYN` interpreter mapped by the kernel at
`AT_BASE` (currently `USER_INTERP_BASE`). Its assembly bootstrap applies only
its own `R_386_RELATIVE` self-relocations, then enters the C loader. The loader
derives the main executable load bias from `AT_PHDR` and `PT_PHDR`, so both
fixed `ET_EXEC` and PIE `ET_DYN` main programs use the same relocation path. It
opens absolute `DT_NEEDED` paths directly, then searches the requesting
object's absolute-only `DT_RUNPATH` or `DT_RPATH`, then `/lib`. It maps
eligible page-aligned read-only `PT_LOAD` pages through the shared read-only
file cache, keeps writable and relocation-bearing pages private, applies
protections, resolves eager relocations, runs DSO initializers, and then calls
the original executable entry with
`(argc, argv, envp)`.

After startup, dynamic programs can call `dlopen()`, `dlsym()`, `dlclose()`,
and `dlerror()` through a loader service table installed into libc before
program entry. Runtime loads reuse the same absolute-path, `RUNPATH`/`RPATH`,
and `/lib` search rules as startup dependencies. `RTLD_NOW` and `RTLD_LAZY`
both resolve eagerly; `RTLD_GLOBAL` exposes active runtime objects through
`RTLD_DEFAULT`, while `RTLD_LOCAL` keeps runtime objects visible through only
their own handles and dependency closures. `dlclose()` is a lifecycle
operation in V2: it drops runtime references and runs finalizers when the last
reference closes, but intentionally leaves mappings and shared file cache pages
in place. Runtime-loaded DSOs are allocated from a bounded
page-aligned arena below `AT_BASE`; arena exhaustion is fatal during startup
and becomes a `dlerror()` failure during runtime `dlopen()`.

Dynamic-link failure paths are intentionally controlled. If the interpreter named by
`PT_INTERP` is missing, the kernel prints `elf: missing interpreter:
/lib/ld-smallos.so`, the launch syscall fails, and the shell prompt returns. If
the interpreter starts but `/lib/libc.so` is missing, `ld-smallos.so` prints
`ld-smallos: library not found: libc.so` and exits with status `127`. The
`make dynlink-negative-smoke` target verifies both paths using temporary
images. Runtime `dlopen()` failures instead return `NULL`, set the per-process
`dlerror()` string, roll back newly-created loader object records, and do not
kill the caller.

V2 still intentionally supports dynamic applications rather than full Linux
loader semantics. There is no lazy PLT binding, TLS, `RTLD_NEXT`, symbol
versioning, environment/config search path, ASLR, or aggressive DSO unload yet.
Static ELFs remain a first-class fallback path.

---

# Invariants

The following must remain true:

- `ext2_init()` must run after the storage policy chooses ATA, USB storage, or boot RAM fallback, and before any VFS-backed ext2 file load
- legacy `vfs_load_file()` / `ext2_load()` results must be copied before another ext2 load reuses the shared buffer
- named ELF loading must keep using `vfs_load_file_owned()` so process creation never maps from the shared ext2 load buffer
- every user process must have a valid kernel stack frame before ring-3 entry
- `tss_set_kernel_stack()` must match the process that will next return from ring 3 into the kernel
  - this is enforced by `elf_user_task_bootstrap()` on first entry, not by the earlier `elf_run_image()` setup path
- timer IRQ and syscall-yield paths must pass the scheduler the true resume-frame base, `esp - 8`, not raw `esp`
- the scheduler must preserve that real saved resume ESP instead of letting `sched_switch()` overwrite it with the scheduler's own C call-frame ESP
- `process_destroy()` must not run until a safe stack is active and the task is already `PROCESS_STATE_ZOMBIE`

---

# Failure Modes

## Program launch failed

Likely causes:

- ext2 file not found
- ext2 load failed
- ELF magic invalid
- PMM frame allocation failure during segment or stack setup

## Returns to the wrong process or faults on exit

Likely causes:

- wrong resume ESP passed to the scheduler instead of `esp - 8`
- scheduler save slot allowed to overwrite the real interrupt/syscall resume ESP
- task destroyed before switching off its own kernel stack

## User process starts but syscalls behave oddly

Likely causes:

- kernel stack in TSS does not match the active user process
- `process_get_current()` points at the wrong process in the hybrid path

## Shell input behaves incorrectly after process exit

Likely causes:

- foreground reader/group not set/cleared correctly around interactive runs
- keyboard routing not falling back from foreground owner to `sched_current()` correctly

---

# Current Status

The execution model is fully scheduler-owned.

- scheduler-owned shell task
- scheduler-owned reaper task — frees unclaimed zombie processes automatically
- timer-driven preemption
- `SYS_YIELD`, `SYS_EXEC`, `SYS_FORK`, `SYS_EXECVE`, `SYS_EXIT` all scheduler-owned
- ELF processes have real per-process page directories
- foreground commands wait with `process_wait()`; `bg` children are reaped automatically; `SYS_EXEC` and `SYS_FORK` children are parent-waitable with `waitpid()` and can report stopped state through `WUNTRACED`; user-shell `bg` children are shell-owned jobs until `fg` or `kill`
- no known zombie or frame leaks
