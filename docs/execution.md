# Execution Model

This document defines how programs are invoked, loaded, scheduled, and executed in the system.

---

# Overview

Programs can be executed through three paths:

```text
run      → built-in C functions (kernel-linked, direct call, ring 0)
runimg   → descriptor-based image execution (ring 0)
runelf   → ELF programs loaded from FAT16 disk partition into per-process address space (ring 3)
```

`runelf` is the primary user-program path. The execution model is currently transitional:

* the **shell** runs as a real kernel task created at boot and entered through `sched_start()`
* **keyboard IRQ1** no longer mutates shell/editor state directly; it queues shell events which the shell task drains in normal task context
* **ELF user programs** still run through the foreground `setjmp`/`longjmp` path and are **not yet** scheduler-owned tasks

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

Direct function pointer call. No page directory change. Runs in ring 0 in the kernel's address space.

## 2. Image Programs (`runimg`)

Descriptor table of function pointers. Transitional — largely superseded by ELF execution.

## 3. ELF Programs (`runelf`)

The current launch path:

```text
elf_run_named(name, argc, argv)
  ↓
fat16_load(name, &size)
  → search FAT16 root directory (case-insensitive 8.3 match)
  → follow FAT16 cluster chain via ATA PIO reads
  → write file into static s_load_buf[256 KB] buffer
  → return pointer to buffer
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
    copy p_filesz bytes from s_load_buf into frames via physical addresses
    [s_load_buf is now safe to reuse — data is in PMM frames]
  ↓
allocate + map user stack:
    stack_frame = pmm_alloc_frame()
    paging_map_page(pd, 0xBFFFF000, stack_frame, PAGE_USER | PAGE_WRITE)
  ↓
proc->kernel_stack_frame = pmm_alloc_frame()
tss_set_kernel_stack(kernel_stack_frame + PAGE_SIZE)
  ↓
save parent context:
    s_parent_proc = process_get_current()
    s_parent_esp0 = parent ? parent->ksf+PAGE_SIZE : 0x90000
  ↓
process_set_current(proc)
proc->state = RUNNING
  ↓
setjmp(proc->exit_ctx)           → save kernel context for sys_exit return
  ↓
keyboard_set_process_mode(1)
paging_switch(pd)
  ↓
elf_enter_ring3(entry, USER_STACK_TOP, argc, argv)
  → copy argv strings onto user stack (ring-3 accessible)
  → build argv pointer array on user stack
  → build cdecl call frame: [ret=0, argc, argv]
  → load DS/ES/FS/GS = SEG_USER_DATA (0x23)
  → push iret frame: SS=0x23, ESP, EFLAGS, CS=0x1B, EIP=e_entry
  → iret  → CPU switches to ring 3
```

---

# SYS_EXEC — Nested ELF Execution

A running user process can spawn a named child via `SYS_EXEC`:

```text
sys_exec("hello", argc, argv)      [ring-3 int 0x80]
  ↓
sys_exec_impl()
  copy name to kernel stack buffer before disk/file lookup
  ↓
elf_run_named(kname, argc, argv)   [same path as runelf above]
  ↓
[child runs; parent suspended at setjmp in elf_run_image on parent's kernel stack]
  ↓
child calls sys_exit() → elf_process_exit()
  paging_switch(s_parent_proc->pd)   ← parent's PD, NOT kernel PD
  tss_set_kernel_stack(s_parent_esp0)
  process_destroy(child)
  process_set_current(s_parent_proc)
  sti
  longjmp(child->exit_ctx, 1)
  ↓
setjmp returns in elf_run_image → returns 1
sys_exec_impl returns 0 → iretd → parent resumes in ring 3
```

**CR3 rule**: `elf_process_exit` must switch to the parent's page directory before `longjmp`. After `longjmp` the `iretd` in `isr128_stub` jumps to the parent's ring-3 EIP at `0x400000` — only mapped in the parent's PD.

**One-deep only**: `s_parent_proc` and `s_parent_esp0` are single statics.

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

Boot sequence:

```text
kernel_main()
  sched_init()
  ata_init()
  fat16_init(FAT16_LBA)
  process_create_kernel_task("shell", shell_task_main)
  sched_enqueue(shell_proc)
  sti
  sched_start(shell_proc)
```

---

# Ring 3 Privilege Model

ELF programs execute at CPL=3:

* Kernel pages (no `PAGE_USER`) are inaccessible to ring-3 code
* User pages (`PAGE_USER | PAGE_WRITE`) at `0x400000+` and `0xBFFFF000` are readable and writable
* `int 0x80` is reachable from ring 3 (IDT gate DPL=3)
* On `int 0x80`, CPU reads TSS.SS0/ESP0 and switches to the process's kernel stack

---

# Argument Passing

```text
runelf hello arg1 arg2

Stack layout before iret (top of user stack, growing down):
  "hello\0"          string data
  "arg1\0"
  "arg2\0"
  [padding to 4B]
  user_argv[0]       → "hello"
  user_argv[1]       → "arg1"
  user_argv[2]       → "arg2"
  user_argv[3]       → 0
  argv ptr           → user_argv
  argc = 3
  return addr = 0    ← ESP after iret
```

---

# Return Path

After `sys_exit`:

```text
ring 3 user program
  ↓
int 0x80
  ↓
syscall_handler_main()
  ↓
elf_process_exit()
  proc->state = EXITED
  keyboard_set_process_mode(0)
  paging_switch(parent_pd or kernel_pd)
  tss_set_kernel_stack(parent_esp0)
  process_destroy(proc)
  process_set_current(parent)
  sti
  longjmp(exit_ctx, 1)
  ↓
setjmp returns in parent context
  ↓
paging_switch(parent_pd or kernel_pd already active)
  ↓
shell or parent user process resumes
```

The child process never returns through the interrupted `iretd` path. `elf_process_exit` leaves via `longjmp`, so it must restore the correct parent state before jumping.

---

# Current Hybrid Model

The current design intentionally mixes two models:

## Scheduler-owned kernel task

* shell task created with `process_create_kernel_task()`
* entered through `sched_start()`
* preempted by timer IRQ
* resumed through `sched_switch()`

## Foreground ELF process

* loaded and entered directly by `elf_run_image()`
* not enqueued into the scheduler
* returns through `elf_process_exit()` → `longjmp()`

This hybrid arrangement works, but it is transitional.

---

# Constraints / Invariants

* `fat16_load()` returns a pointer into a static load buffer — callers must copy ELF segment contents before another FAT16 load
* process-owned frames come from PMM and are reclaimed on exit
* `tss_set_kernel_stack()` must always match the currently active ring-3 process's kernel stack
* the shell resumes with interrupts enabled because `elf_process_exit()` executes `sti` before `longjmp`
* `SYS_EXEC` is one-deep only

---

# Future Direction

The next architectural step is:

```text
runelf → create process → enqueue in scheduler → remove foreground setjmp/longjmp path
```

That will unify the shell and user-program execution model.