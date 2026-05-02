# Execution Model

This document describes how commands are dispatched, how ELF programs are loaded, and how the current scheduler / syscall model actually behaves.

For the C runtime contract exposed to those ELF programs, including cwd,
`errno`, fd wrappers, stdio, and directory APIs, see
[`docs/user-runtime.md`](user-runtime.md).

It reflects the current code in:

- `src/shell/shell.c`
- `src/shell/commands.c`
- `src/exec/elf_loader.c`
- `src/kernel/process.c`
- `src/kernel/scheduler.c`
- `src/kernel/syscall.c`

---

# Overview

There are currently **two external program paths**:

```text
shell command
  → runelf <name> [args]
  → vfs_load_file()
  → elf_run_image()
  → sched_enqueue(proc)
  → process_wait(proc)
  → scheduler enters child via elf_user_task_bootstrap()
  → ring-3 entry via iret
  → syscalls via int 0x80
  → sys_exit() → sched_exit_current()
  → child becomes ZOMBIE, waiter destroys it

shell command
  → runelf_nowait <name> [args]
  → vfs_load_file()
  → elf_run_image()
  → sched_enqueue(proc)
  → return immediately
```

Important current-state facts:

- the **shell** is a scheduler-owned kernel task created at boot
- **keyboard IRQ1** decodes scancodes and calls a registered `keyboard_consumer_fn` — it makes no routing decisions itself
- the **shell consumer** (`shell_key_consumer` in `shell.c`) enqueues `shell_event_t` entries; the shell task drains them in `shell_poll()` outside IRQ context
- the **process consumer** (`process_key_consumer` in `process.c`) pushes ASCII into `kb_buf`; non-ASCII key events are ignored
- consumer ownership transfers via `process_set_foreground()` — shell consumer at boot, process consumer while a user process holds the foreground, shell consumer restored on exit
- **ELF user programs** are loaded into their own page directory and do execute in ring 3
- ELF launch and exit are now scheduler-owned: `elf_run_image()` seeds a bootstrap context, enqueues the task, and returns `process_t*`
- the scheduler supports kernel tasks, ELF tasks, voluntary yielding, timer-driven sleeping, and timer-driven switching; `runelf` blocks with `process_wait()`, while `runelf_nowait` returns immediately
- user ELFs now have a small freestanding runtime layer with a heap allocator,
  fd-backed console streams, streaming VFS-backed file handles,
  `stat`/`rename`/`unlink`, `lseek`, and socket wrappers, which is enough for
  compiler-style tools and small network services
- the shipped `tools/tcc.elf` compiler binary links the generic SmallOS `user_crt0` adapter and runs TinyCC's normal `main`, can compile guest C sources from FAT16, write the results back to disk, and then those generated ELFs can be executed immediately
- QEMU user networking is still the default for `make run` / `make test`, but `make run-tap` switches the NIC onto a host TAP device for bridged or routed networking beyond QEMU's built-in NAT
- `pinggw` only proves the QEMU gateway works; `pingpublic` and `netcheck` are the manual probes for "can I reach beyond the gateway?"
- `pingpublic` now routes the echo request through the QEMU gateway instead of ARPing the public IP directly
- `apps/services/tcpecho.elf`, `apps/services/sockeof.elf`, and `apps/services/ftpd.elf` are the current guest-side TCP smoke apps; they run as normal ELFs and are exercised through QEMU hostfwd on the guest service ports
- `sockeof.elf` listens on `2463` in the guest and is driven by `make socket-eof-smoke` to verify payload-before-EOF, `POLLHUP`, and post-EOF response writes
- `ftpd.elf` listens on `2121` in the guest and expects passive data connections on `30000`; host-side clients such as `lftp`, WinSCP, and FileZilla should use passive mode

---

# Command Flow

```text
keyboard IRQ1
  ↓
keyboard_handle_irq()
  ↓
decode scancode → key_event_t
  ↓
call s_consumer(ev)   ← registered keyboard_consumer_fn
  ↓
shell_key_consumer()
  ↓
enqueue shell_event_t (char, backspace, arrows, history, etc.)
  ↓
shell_task_main()
  ↓
shell_poll()
  ↓
line_editor
  ↓
[Enter pressed]
  ↓
parse_command()
  ↓
commands_execute()
  ↓
cmd_runelf()
  ↓
elf_run_named()
```

The shell task itself is created in `kernel_main()` with `process_create_kernel_task("shell", shell_task_main)`, enqueued with `sched_enqueue()`, then entered with `sched_start()`.

---

# Supported Program Paths

## 1. Built-in shell commands

Commands like `help`, `clear`, `meminfo`, `fsls [path]`, `ls [pattern]`, `fsread`, `cat`, `cd`, `pwd`, `mkdir`, `rmdir`, `rm`, `touch`, `cp`, and `mv` are normal kernel C functions dispatched by `commands_execute()`.

Commands like `echo`, `about`, `uptime`, `halt`, and `reboot` are thin kernel wrappers that launch same-named ELFs and wait for them to finish. The command names stay in the shell, but the behavior now lives in user space.

They:

- run in ring 0
- run in the kernel address space
- do not switch page directories
- do not create a new `process_t`

## 2. ELF programs (`runelf`)

`runelf` is the current external-user-program path.

Command handler:

```c
if (!elf_run_named(cmd->argv[1], cmd->argc - 1, &cmd->argv[1])) {
    terminal_puts("runelf: failed\n");
}
```

That means `argv[0]` inside the ELF process is the program name passed to `runelf`, and the rest of the shell tokens follow as normal argv entries. Nested paths such as `runelf apps/demo/hello alpha beta` are supported as long as the FAT16 image contains the target entry.

There is **no active `runimg` command path** in the current shell command table.

The same path is used by the guest TinyCC smoke tests:

```text
runelf tools/tcc.elf -nostdlib -o out.elf samples/tccmath.c
runelf out.elf
```

The test suite uses this flow to compile several focused C samples inside the guest and then run the generated ELFs. The produced binaries are written back under the `apps/tests/` subtree so the shell can execute them by path.

TinyCC's runtime expectations are part of the user runtime contract in
[`docs/user-runtime.md`](user-runtime.md).

For the TCP bring-up path, the shell can also launch a long-lived service
with `runelf_nowait apps/services/tcpecho`, `runelf_nowait
apps/services/sockeof`, or `runelf_nowait apps/services/ftpd`. Those programs
bind and listen inside the guest, and you connect to them from the host
through QEMU `hostfwd`.

The FTP service uses passive data connections, so a host-driven smoke needs
both the control port and passive data port forwarded:

```text
hostfwd=tcp::2121-:2121,hostfwd=tcp::30000-:30000
```

`make ftp-smoke` sets those forwards, launches `ftpd`, and verifies login,
negative path replies, directory listing, download, upload readback, delete,
and `RMD` cleanup.

---

# ELF Load Path

## Name lookup and file read

`elf_run_named(name, argc, argv)` does:

```text
vfs_load_file(name, &size)
  → backend file lookup
  → follow cluster chain with ATA PIO reads
  → copy file into static FAT16 load buffer
  → return pointer to buffer
```

Important invariant:

- `fat16_load()` returns a pointer into a **static internal buffer**
- callers must copy what they need before another FAT16 load reuses that buffer

`elf_run_named()` then passes that image pointer to `elf_run_image()`.

## ELF validation and process creation

`elf_run_image()`:

- validates ELF magic
- allocates a fresh `process_t` with `process_create("elf")`
- allocates a fresh page directory with `process_pd_create()`
- maps each `PT_LOAD` segment into user memory using PMM frames
- copies file-backed bytes from the FAT16 load buffer into those frames
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

`tss_set_kernel_stack()` is **not** called during `elf_run_image()` setup. That update happens later inside `elf_user_task_bootstrap()`, at the moment the scheduler first enters the new process. This avoids clobbering the currently running task's ESP0 during async launch paths such as `runelf_nowait` and `SYS_EXEC`.

For `runelf`, the shell then calls `process_wait(proc)` and blocks until the child reaches `PROCESS_STATE_ZOMBIE`. For `runelf_nowait`, the shell returns immediately after enqueue.

The shell-side launch helpers keep their larger scripted command tables out of
the shell task's 4 KB kernel stack. That matters for `shelltest`, `selftest`,
and `runelf_nowait`, which run through the same foreground shell context while
launching and waiting on many child tasks.

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

The old explicit parent-tracking statics are gone. The current design relies on scheduler ownership, foreground input ownership, and automatic zombie reaping:

- `runelf` launches the child, then waits with `process_wait(proc)`
- `runelf_nowait` launches the child and returns immediately; the reaper task frees it after exit
- `SYS_EXEC` children are also unclaimed — freed by the reaper
- interactive foreground input is tracked with `process_set_foreground(proc)` / `process_get_foreground()`
- process destruction is either explicit via `process_wait()` or automatic via `sched_reap_zombies()`

---

# SYS_EXEC Current Reality

`SYS_EXEC` is in transition.

`sys_exec_impl()` in `src/kernel/syscall.c` is now fully **spawn-style**:

- copy program name into a kernel buffer
- call `elf_run_named()`
- return `0` on success, or a negative errno such as `-ENOENT` / `-EFAULT` on failure

`elf_run_named()` follows the same scheduler-owned ELF launch path as shell commands: create the process, seed its bootstrap context, enqueue it, and return immediately.

The file, console, and socket syscalls used by shell tools, TinyCC, and the
FTP/TCP smoke apps now share the process-owned handle table in `process.c`.
Each handle has readable/writable/dirty state plus ops for `read`, `write`,
`seek`, `poll`, `flush`, and `close`.
Each process also carries cwd state, so user path syscalls resolve relative
paths before entering VFS or ELF loading.
FAT16-backed file behavior and path operations sit behind `vfs.c`, so
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
  timer_init(SMALLOS_TIMER_HZ)
  idt_init()
  sched_init()
  ata_init()
  fat16_init()
  create shell kernel task
  sched_enqueue(shell_proc)
  process_start_reaper()    ← creates and enqueues reaper task
  sti
  sched_start(shell_proc)
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
- **foreground `runelf` waits with `process_wait()`**
- **`runelf_nowait` and `SYS_EXEC` children are reaped automatically by the reaper task**

---

# Process Ownership Rules

There are currently two related ownership concepts.

`process_get_current()` follows the scheduler-owned current task. Interactive input routing uses foreground ownership via `process_set_foreground()` / `process_get_foreground()`.

That means:

- during normal shell execution, the current process is the scheduler-owned shell task
- while the shell is waiting on a foreground ELF, keyboard routing still follows the foreground owner first
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

When `runelf apps/demo/hello a b` is invoked, the ELF sees:

- `argc = 3`
- `argv[0] = "hello"`
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
- `_start` receives `argc` and `argv`
- `argv[argc]` is `NULL`
- `argv` strings and the pointer array live on the initial user stack
- there is no `envp` yet
- returning from `_start` is unsupported unless a CRT layer converts the return value into `sys_exit`

`src/user/user_crt0.c` is that CRT layer for hosted-ish programs. It keeps the
kernel ABI at `_start(argc, argv)`, calls `main(argc, argv)`, and exits with
the returned status. TinyCC is linked this way so its upstream `main` path can
run normally.

---

# Invariants

The following must remain true:

- `fat16_init()` must run after `ata_init()` and before any VFS-backed FAT16 file load
- `vfs_load_file()` / `fat16_load()` results must be copied before another FAT16 load reuses the static buffer
- every user process must have a valid kernel stack frame before ring-3 entry
- `tss_set_kernel_stack()` must match the process that will next return from ring 3 into the kernel
  - this is enforced by `elf_user_task_bootstrap()` on first entry, not by the earlier `elf_run_image()` setup path
- timer IRQ and syscall-yield paths must pass the scheduler the true resume-frame base, `esp - 8`, not raw `esp`
- the scheduler must preserve that real saved resume ESP instead of letting `sched_switch()` overwrite it with the scheduler's own C call-frame ESP
- `process_destroy()` must not run until a safe stack is active and the task is already `PROCESS_STATE_ZOMBIE`

---

# Failure Modes

## `runelf: failed`

Likely causes:

- FAT16 file not found
- FAT16 load failed
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

- foreground owner not set/cleared correctly around interactive runs
- keyboard routing not falling back from foreground owner to `sched_current()` correctly

---

# Current Status

The execution model is fully scheduler-owned.

- scheduler-owned shell task
- scheduler-owned reaper task — frees unclaimed zombie processes automatically
- timer-driven preemption
- `SYS_YIELD`, `SYS_EXEC`, `SYS_EXIT` all scheduler-owned
- ELF processes have real per-process page directories
- foreground `runelf` waits with `process_wait()`; `runelf_nowait` and `SYS_EXEC` children are reaped automatically
- no known zombie or frame leaks
