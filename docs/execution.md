# Execution Model

This document describes how commands are dispatched, how ELF programs are loaded, and how the current scheduler / syscall model actually behaves.

It reflects the current code in:

- `src/shell/shell.c`
- `src/shell/commands.c`
- `src/exec/elf_loader.c`
- `src/kernel/process.c`
- `src/kernel/scheduler.c`
- `src/kernel/syscall.c`

---

# Overview

There is currently **one real external program path**:

```text
shell command
  → runelf <name> [args]
  → fat16_load()
  → elf_run_image()
  → ring-3 entry via iret
  → syscalls via int 0x80
  → sys_exit() → elf_process_exit()
  → return to parent context via longjmp
```

Important current-state facts:

- the **shell** is a scheduler-owned kernel task created at boot
- **keyboard IRQ1** does not mutate shell/editor state directly; it queues events that the shell task drains later
- **ELF user programs** are loaded into their own page directory and do execute in ring 3
- **ELF launch/exit still uses the older foreground `setjmp` / `longjmp` path**
- the scheduler exists and supports kernel tasks plus voluntary / timer-driven switching; the foreground ELF path still uses `setjmp` / `longjmp`, but `elf_run_image()` already seeds a valid scheduler entry context for the process with `elf_seed_sched_context()`

---

# Command Flow

```text
keyboard IRQ1
  ↓
keyboard_handle_irq()
  ↓
queue shell event (char, backspace, arrows, history, etc.)
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

Commands like `help`, `clear`, `meminfo`, `fsls`, and `fsread` are normal kernel C functions dispatched by `commands_execute()`.

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

That means `argv[0]` inside the ELF process is the program name passed to `runelf`, and the rest of the shell tokens follow as normal argv entries.

There is **no active `runimg` command path** in the current shell command table.

---

# ELF Load Path

## Name lookup and file read

`elf_run_named(name, argc, argv)` does:

```text
fat16_load(name, &size)
  → case-insensitive FAT16 root-directory lookup
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

After mappings are created, `elf_run_image()` prepares for ring-3 entry.

The code currently does all of the following:

1. allocate the process kernel stack
2. call `tss_set_kernel_stack(proc->kernel_stack_frame + PAGE_SIZE)`
3. call `elf_seed_sched_context(proc, eh->e_entry, argc, argv)`
4. save the parent foreground context in `s_parent_proc` / `s_parent_esp0`
5. set `process_set_current(proc)`
6. mark the process `PROCESS_STATE_RUNNING`
7. save a return point with `setjmp(proc->exit_ctx)`
8. switch to the process page directory
9. call `elf_enter_ring3()` using the kernel-side argv copy in `proc->user_argv`

`elf_enter_ring3()` then:

- copies argv strings onto the user stack
- builds the `argv[]` pointer array on that same user stack
- pushes a fake return address, `argc`, and `argv`
- loads user data segments
- pushes an `iret` frame
- executes `iret`

That transitions the CPU from CPL 0 to CPL 3 and begins execution at `e_entry`.

---

# Parent / Child Tracking

Foreground ELF execution currently uses two file-static globals in `elf_loader.c`:

- `s_parent_proc`
- `s_parent_esp0`

These track the context that must be restored when the foreground ELF exits.

Current behavior:

- top-level `runelf` from the shell usually has a scheduler-owned kernel-task parent
- nested `SYS_EXEC` from a user process can have a ring-3 parent
- only a **single parent chain** is tracked explicitly by these globals

This is why nested process behavior remains part of the transitional design.

---

# SYS_EXEC Current Reality

`SYS_EXEC` is in transition.

## What the syscall layer says

`sys_exec_impl()` in `src/kernel/syscall.c` is documented as **spawn-style**:

- copy program name into a kernel buffer
- call `elf_run_named()`
- return `0` on success, `-1` on failure

## What the core ELF path still does

`elf_run_named()` still enters the child through the older foreground path in `elf_run_image()`:

- save parent return point with `setjmp(proc->exit_ctx)`
- enter child with `iret`
- child exits through `elf_process_exit()`
- return to the saved parent context with `longjmp`

So the accurate current statement is:

- **the syscall API is being moved toward spawn-style semantics**
- **the actual launch / exit machinery still uses the foreground `setjmp` / `longjmp` model**

Until the ELF path is fully scheduler-owned, documentation should describe `SYS_EXEC` as transitional rather than fully converted.

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
elf_process_exit()
```

`elf_process_exit()` currently does this:

1. get the current foreground process via `process_get_current()`
2. mark it `PROCESS_STATE_EXITED`
3. disable process keyboard mode with `keyboard_set_process_mode(0)`
4. copy `proc->exit_ctx` into a local stack buffer
5. switch CR3 back to the parent PD if there is one, otherwise the kernel PD
6. restore the parent kernel stack in the TSS with `tss_set_kernel_stack(s_parent_esp0)`
7. destroy the child process with `process_destroy(proc)`
8. restore `process_set_current(s_parent_proc)`
9. execute `sti`
10. `longjmp(exit_ctx, 1)` back to the saved parent context

Important invariant:

- the jump target must be copied out of `process_t` **before** `process_destroy()` because destroying the process frees the frame containing `exit_ctx`

Important CR3 rule:

- if the parent is a user process, `elf_process_exit()` must restore the **parent's** page directory before the `longjmp`
- the parent's ring-3 return address is only valid in that address space
- switching to the kernel PD first would make the resumed user return path invalid

---

# Scheduler Interaction

The scheduler is real and preemptive, but ELF launch is still not fully under scheduler ownership.

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
  timer_init(100)
  idt_init()
  sched_init()
  ata_init()
  fat16_init()
  create shell kernel task
  sched_enqueue(shell_proc)
  sti
  sched_start(shell_proc)
```

## What the scheduler owns today

The scheduler directly owns:

- kernel tasks created with `process_create_kernel_task()`
- voluntary yields via `SYS_YIELD` → `sched_yield_now()`
- timer-driven preemption via `sched_tick()`

## What it does not fully own yet

The foreground ELF launch path in `elf_run_image()` still:

- saves its return point with `setjmp`
- switches into the child directly with `iret`
- returns via `elf_process_exit()` → `longjmp`

`elf_loader.c` already seeds a scheduler context for user ELF entry via `elf_seed_sched_context()`. That code copies argv into `process_t` storage, builds a valid `sched_esp` on the process kernel stack, and seeds first scheduler re-entry through `elf_user_task_bootstrap()`.

The current launch path still does **not enqueue the process yet**, so the active execution path remains:

- **foreground ELF launch/exit still uses the direct `setjmp` / `iret` / `longjmp` path**
- **scheduler entry support for user ELFs is already present and valid, but not yet the primary launch path**

---

# Process Ownership Rules

There are currently two notions of “current process” in the hybrid model.

`process_get_current()` works like this:

- if `process_set_current()` has installed a foreground ELF process, return that
- otherwise return `sched_current()`

That means:

- during normal shell execution, the current process is the scheduler-owned shell task
- while a foreground ELF is active, it overrides the scheduler-owned current task for code paths that ask for `process_get_current()`

This is a deliberate transitional rule used by syscalls, exit handling, and keyboard process mode.

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

When `runelf hello a b` is invoked, the ELF sees:

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

---

# Invariants

The following must remain true:

- `fat16_init()` must run after `ata_init()` and before any FAT16 file load
- `fat16_load()` results must be copied before another FAT16 load reuses the static buffer
- every user process must have a valid kernel stack frame before ring-3 entry
- `tss_set_kernel_stack()` must match the process that will next return from ring 3 into the kernel
- `elf_process_exit()` must restore the correct parent CR3 before `longjmp`
- `process_destroy()` must not run until any needed `exit_ctx` has been copied out
- `process_get_current()` returning the foreground ELF during its active window is required by the current hybrid design

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

- wrong parent CR3 restored before `longjmp`
- wrong TSS ESP0 restored before resumed ring-3 activity
- `exit_ctx` used after destroying the child process object

## User process starts but syscalls behave oddly

Likely causes:

- kernel stack in TSS does not match the active user process
- `process_get_current()` points at the wrong process in the hybrid path

## Shell input behaves incorrectly after process exit

Likely causes:

- `keyboard_set_process_mode(0)` not restored on exit
- interrupts not re-enabled before returning to the parent context

---

# Transitional Status

The execution model is intentionally mid-migration.

Already true:

- scheduler-owned shell task
- timer-driven preemption exists
- `SYS_YIELD` exists
- `SYS_EXEC` exists
- ELF processes have real per-process page directories
- `elf_run_image()` already calls `elf_seed_sched_context()`
- user tasks already have valid seeded scheduler entry stacks that re-enter through `elf_user_task_bootstrap()`

Not finished yet:

- ELF launch through `sched_enqueue()` as the primary path
- ELF exit through scheduler-owned task teardown rather than foreground `longjmp`
- fully consistent `SYS_EXEC` semantics across syscall docs, headers, and implementation comments

---

# Future Direction

The intended next step is:

```text
runelf / sys_exec
  → create process
  → seed user bootstrap context (`elf_seed_sched_context()`)
  → sched_enqueue(proc)
  → scheduler owns first entry, preemption, and exit lifecycle
  → remove foreground setjmp/longjmp path
```

That would unify shell tasks and user ELF tasks under one execution model and eliminate the remaining parent-context special cases.
