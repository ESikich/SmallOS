# Execution Model

This document defines how programs are invoked, loaded, scheduled, and executed in the system.

---

# Overview

Programs can be executed through three paths:

```text
run      → built-in C functions (kernel-linked, direct call, ring 0)
runimg   → descriptor-based image execution (ring 0)
runelf   → ELF programs loaded from ramdisk into per-process address space (ring 3)
```

`runelf` is still the primary user-program path, but the execution model is currently transitional:

* the **shell** now runs as a real kernel task created at boot and entered through `sched_start()`
* **keyboard IRQ1** no longer mutates shell/editor state directly; it queues shell events which the shell task drains in normal task context
* **ELF user programs** still run through the older foreground `setjmp`/`longjmp` path and are **not yet** scheduler-owned tasks

That hybrid model is intentional for the current build.

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
line_editor (insert, delete, cursor movement, history)
  ↓
[Enter pressed]
  ↓
parse_command()     tokenizes input in place into fixed argv[MAX_ARGS] entries
  ↓
commands_execute()  linear search of command table
  ↓
dispatch to handler
```

---

# Execution Types

## 1. Built-in Programs (`run`)

Direct function pointer call. No page directory change. Runs in ring 0 in the kernel's address space. Use case: simple utilities and debugging.

## 2. Image Programs (`runimg`)

Descriptor table of function pointers. Transitional — largely superseded by ELF execution.

## 3. ELF Programs (`runelf`)

The current launch path is:

```text
elf_run_named(name, argc, argv)
  ↓
ramdisk_find(name)               → pointer into ramdisk image at 0x10000
  ↓
elf_run_image(data, argc, argv)
  ↓
validate ELF magic (0x7F 'E' 'L' 'F')
  ↓
process_create("elf")            → allocate process_t from PMM frame
  ↓
process_pd_create()
  → pmm_alloc_frame() → 4KB-aligned page directory
  → copy kernel PD entries (indices 0, 2–1023) for shared kernel access
  → PD index 1 left empty (private user region)
  ↓
for each PT_LOAD segment:
    pages = ceil(p_memsz / 4096)
    for each page:
        frame = pmm_alloc_frame()
        mem_zero(frame, 4096)
        paging_map_page(pd, p_vaddr + p*4096, frame, PAGE_USER | PAGE_WRITE)
    copy p_filesz bytes from ramdisk into frames via physical addresses
  ↓
allocate + map user stack:
    stack_frame = pmm_alloc_frame()
    paging_map_page(pd, 0xBFFFF000, stack_frame, PAGE_USER | PAGE_WRITE)
  ↓
proc->kernel_stack_frame = pmm_alloc_frame()
tss_set_kernel_stack(kernel_stack_frame + PAGE_SIZE) → TSS ESP0 for ring-3 syscalls
  ↓
process_set_current(proc)        → foreground user process pointer
proc->state = RUNNING
  ↓
setjmp(proc->exit_ctx)           → save kernel context for sys_exit return
  ↓
keyboard_set_process_mode(1)     → route keystrokes to process buffer
paging_switch(pd)                → load process CR3, flush TLB
  ↓
elf_enter_ring3(entry, USER_STACK_TOP, argc, argv)
  → copy argv strings onto user stack (ring-3 accessible)
  → build argv pointer array on user stack
  → build cdecl call frame: [ret=0, argc, argv]
  → load DS/ES/FS/GS = SEG_USER_DATA (0x23)
  → push iret frame: SS=0x23, ESP, EFLAGS, CS=0x1B, EIP=e_entry
  → iret  → CPU switches to ring 3
```

After that, the process runs until it exits via `sys_exit()`. Timer interrupts still occur while the process is running, but this build does **not** yet enqueue the ELF process into the round-robin scheduler.

---

# Scheduler Interaction

The scheduler is timer-driven and preemptive:

```text
irq0_stub
  ↓
irq0_handler_main(esp)
  timer_handle_irq()
  send EOI to PIC
  sched_tick(esp)
```

In the current build, the scheduler owns **kernel tasks** such as the shell. Boot now looks like this:

```text
kernel_main()
  ↓
sched_init()
  ↓
ramdisk_init()
  ↓
process_create_kernel_task("shell", shell_task_main)
  ↓
sched_enqueue(shell_proc)
  ↓
sti
  ↓
sched_start(shell_proc)
```

Important details:

* the shell is now a real `process_t` with its own PMM-backed kernel stack
* `process_create_kernel_task()` seeds an initial `sched_esp` so the first `sched_switch()` returns into a kernel-task bootstrap helper
* this build intentionally does **not** schedule `runelf` processes yet; that conversion is the next architectural step

---

# Ring 3 Privilege Model

ELF programs execute at CPL=3. The hardware enforces the following:

* Kernel pages (no `PAGE_USER` flag) are inaccessible to ring-3 code
* User pages (`PAGE_USER | PAGE_WRITE`) at `0x400000+` and `0xBFFFF000` are readable and writable
* `int 0x80` is reachable from ring 3 because the IDT gate has DPL=3 (`IDT_FLAG_INT_GATE_USER`)
* On `int 0x80`, the CPU reads TSS.SS0 and TSS.ESP0 to find the ring-0 stack; that stack is set from `kernel_stack_frame + PAGE_SIZE` before launch

---

# Argument Passing

`parse_command()` does not allocate from the kernel heap. It tokenizes the shell input buffer in place by replacing spaces with `\0` and storing pointers to each token in the fixed-size `argv[MAX_ARGS]` array inside `command_t`.

Before `iret`, `elf_enter_ring3()` copies the pointed-to argument strings and the pointer array onto the user stack so they are valid in ring 3:

```text
runelf hello arg1 arg2

Stack layout built before iret (top of user stack, growing down):
  "hello\0"          string data (ring-3 accessible)
  "arg1\0"
  "arg2\0"
  [padding to 4B]
  user_argv[0]       → "hello" (user stack address)
  user_argv[1]       → "arg1"
  user_argv[2]       → "arg2"
  user_argv[3]       → 0 (null terminator)
  argv ptr           → user_argv
  argc = 3
  return addr = 0    ← ESP after iret
```

`_start(argc, argv)` receives the correct values via standard cdecl calling convention.

---

# Syscall Integration

ELF programs interact with the kernel via `int 0x80`. See `syscalls.md` for the full ABI.

1. Ring-3 code executes `int 0x80`
2. CPU checks IDT[128].DPL=3 — allowed from ring 3
3. CPU loads SS0/ESP0 from the TSS — switches to the process's kernel stack
4. CPU pushes SS, ESP, EFLAGS, CS, EIP onto that kernel stack
5. `isr128_stub` saves registers, loads kernel segments (`0x10`), and calls `syscall_handler_main`
6. Handler executes and writes the return value to `regs->eax`
7. `iretd` returns to ring 3 — except for `SYS_EXIT`, which takes the foreground exit path described below

Kernel mappings remain present in every process PD because kernel PDEs are shared into each process directory, but ring-3 code still cannot access those mappings directly because they are supervisor-only.

---

# Keyboard Input During Execution

The keyboard path is now split:

* when the shell is active, IRQ1 queues shell events and the shell task drains them with `shell_poll()`
* when a user process is active, `keyboard_set_process_mode(1)` redirects ASCII keystrokes into the 256-byte process input ring buffer for `SYS_READ`

On process exit, `keyboard_set_process_mode(0)` restores shell routing and clears the process input buffer.

The syscall gate is an interrupt gate, so IF is cleared on entry. `sys_read_impl()` re-enables interrupts with `sti` before waiting and restores `cli` before returning so keyboard IRQs can fire during the blocking read.

---

# Address Space During Execution

```text
process PD (active while that process is running):
  0x000000–0x3FFFFF   shared kernel mappings (supervisor-only)
  0x400000–0x7FFFFF   user ELF pages (private, PAGE_USER | PAGE_WRITE)
  0x800000–0xBFFEFFFF unmapped
  0xBFFFF000          user stack page (private, PAGE_USER | PAGE_WRITE)
  PD indices 2–1023   additional shared kernel mappings (supervisor-only)
```

---

# Exit Path

`sys_exit()` is the supported way for a ring-3 program to terminate. A bare `ret` from `_start` has no valid return address and will fault.

`elf_process_exit()` runs in ring 0 from inside the syscall handler:

1. Sets `proc->state = EXITED`
2. Restores keyboard routing to shell mode
3. Switches CR3 back to the kernel page directory
4. Restores TSS ESP0 to `0x90000` for the legacy foreground return path
5. Saves a pointer to `proc->exit_ctx`
6. Calls `process_destroy(proc)` to free the process PD, user frames, kernel stack frame, and `process_t` frame
7. Calls `process_set_current(0)`
8. Executes `sti`
9. `longjmp`s back to the `setjmp` checkpoint in `elf_run_image()`

The explicit `sti` is required because `SYS_EXIT` enters through an interrupt gate, which clears IF. Since the exit path returns through `longjmp()` instead of unwinding back through `iretd`, the shell would otherwise resume with interrupts disabled and the keyboard would appear dead.

When the `longjmp` returns to `elf_run_image()`, that function returns success to the shell command path.

---

# Design Constraints

* No filesystem-backed program loading yet — user programs must live in the ramdisk at build time
* No dynamic linking — user programs are statically linked at `0x400000`
* No `SYS_YIELD` or `SYS_EXEC` yet
* Kernel still trusts user pointers in syscalls (no copy-from-user validation)
* ELF programs are not yet scheduler-owned tasks; the current model is intentionally hybrid

---

# Summary

```text
boot    → sched_init()
        → ramdisk_init()
        → process_create_kernel_task("shell", shell_task_main)
        → sched_enqueue(shell)
        → sti
        → sched_start(shell)

runelf  → ramdisk_find()
        → process_create()
        → process_pd_create()
        → map ELF frames at 0x400000 (PAGE_USER)
        → map stack at 0xBFFFF000 (PAGE_USER)
        → allocate per-process kernel stack
        → tss_set_kernel_stack()
        → process_set_current(proc)
        → setjmp(proc->exit_ctx)
        → keyboard_set_process_mode(1)
        → paging_switch(process_pd)
        → iret into ring 3
        → [program runs; syscalls use int 0x80]
        → sys_exit() → elf_process_exit()
        → paging_switch(kernel_pd)
        → process_destroy(proc)
        → sti
        → longjmp() → return to shell
```
