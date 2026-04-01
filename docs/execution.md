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

`runelf` is the primary user-program path. User processes can also spawn children via `SYS_EXEC`.

The execution model is currently hybrid:

* the **shell** now runs as a real kernel task created at boot and entered through `sched_start()`
* **keyboard IRQ1** no longer mutates shell/editor state directly; it queues shell events which the shell task drains in normal task context
* **ELF user programs** still run through the foreground `setjmp`/`longjmp` path and are **not yet** scheduler-owned tasks

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

### Launch path

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
elf_seed_sched_context()         → copy argv strings into process_t.user_arg_data;
                                   set proc->user_argc, proc->user_argv[]
  ↓
save parent context:
    s_parent_proc = process_get_current()   (0 if shell launched)
    s_parent_esp0 = parent ? parent->kernel_stack_frame + PAGE_SIZE : 0x90000
  ↓
process_set_current(proc)        → foreground user process pointer
proc->state = RUNNING
  ↓
setjmp(proc->exit_ctx)           → save kernel context for sys_exit return
  ↓
keyboard_set_process_mode(1)     → route keystrokes to process buffer
paging_switch(pd)                → load process CR3, flush TLB
  ↓
elf_enter_ring3(entry, USER_STACK_TOP, proc->user_argc, proc->user_argv)
  → copy argv strings from proc->user_argv (kernel copies) onto user stack
  → build argv pointer array on user stack
  → build cdecl call frame: [ret=0, argc, argv]
  → load DS/ES/FS/GS = SEG_USER_DATA (0x23)
  → push iret frame: SS=0x23, ESP, EFLAGS, CS=0x1B, EIP=e_entry
  → iret  → CPU switches to ring 3
```

After that, the process runs until it exits via `sys_exit()`. Timer interrupts still occur while the process is running, but this build does **not** yet enqueue the ELF process into the round-robin scheduler.

---

## 4. SYS_EXEC — launching a child from user space

A running user process can load and run a child ELF:

```text
ring-3: sys_exec("hello", argc, argv)
  ↓
int 0x80 → isr128_stub → syscall_handler_main
  ↓
sys_exec_impl(name, argc, argv)
  copy name from user address to kernel stack buffer kname[]
  (name is a user VA; elf_run_named calls ramdisk_find after CR3 switches,
   so the original pointer would be unmapped at that point)
  ↓
elf_run_named(kname, argc, argv)
  ↓
elf_run_image()   [full launch path above]
  saves s_parent_proc = exec_test's process_t
  saves s_parent_esp0 = exec_test's kernel_stack_frame + PAGE_SIZE
  setjmp(child->exit_ctx)   ← captured on exec_test's kernel stack
  paging_switch(child->pd)
  iret → child runs in ring 3
  ↓
[child runs; exec_test is suspended at setjmp in elf_run_image]
  ↓
child calls sys_exit() → elf_process_exit()
  paging_switch(s_parent_proc->pd)   ← exec_test's PD, not kernel PD
  tss_set_kernel_stack(s_parent_esp0)
  process_destroy(child)
  process_set_current(s_parent_proc)
  sti
  longjmp(child->exit_ctx, 1)
  ↓
setjmp returns 1 in elf_run_image → return 1
  → elf_run_named returns 1
  → sys_exec_impl returns 0 → regs->eax = 0
  ↓
syscall_handler_main returns → isr128_stub iretd
  ↓
exec_test resumes in ring 3 at the instruction after int 0x80
```

**Why `s_parent_proc->pd`, not the kernel PD:** After `longjmp` the return chain traverses kernel code (mapped in all PDs) and reaches `isr128_stub`'s `iretd`. That instruction jumps to exec_test's ring-3 EIP at `0x400000`, which is only mapped in exec_test's page directory. If the kernel PD were active at that point, the `iretd` would cause an immediate page fault.

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
* this build intentionally does **not** schedule `runelf` processes yet; that conversion is a future architectural step

## SYS_YIELD integration

`sys_yield_impl(esp)` receives `(unsigned int)regs` — the `isr128_stub` frame pointer — and calls `sched_yield_now(esp)`. This function resets `s_tick_count` and immediately calls `sched_do_switch(esp)`, the same core used by `sched_tick()`.

The `isr128_stub` frame layout (pusha + 4 segment pushes + esp) is structurally identical to `irq0_stub`. The scheduler therefore saves this ESP as the resume point and resumes the yielding process via `iretd` exactly as it would a timer-preempted context.

---

# Ring 3 Privilege Model

ELF programs execute at CPL=3. The hardware enforces the following:

* Kernel pages (no `PAGE_USER` flag) are inaccessible to ring-3 code
* User pages (`PAGE_USER | PAGE_WRITE`) at `0x400000+` and `0xBFFFF000` are readable and writable
* `int 0x80` is reachable from ring 3 because the IDT gate has DPL=3 (`IDT_FLAG_INT_GATE_USER`)
* On `int 0x80`, the CPU reads TSS.SS0 and TSS.ESP0 to find the ring-0 stack; that stack is set from `kernel_stack_frame + PAGE_SIZE` before launch

---

# Argument Passing

`elf_seed_sched_context()` copies all argv strings into `process_t.user_arg_data` and sets up `proc->user_argv[]` pointers into that buffer — entirely kernel-side, before any CR3 switch. `elf_enter_ring3()` then copies from those kernel copies onto the child's user stack. This ensures argv is valid regardless of whose page directory is active.

For `SYS_EXEC`, the child name is copied from user space into a kernel stack buffer in `sys_exec_impl` before `elf_run_named` is called, for the same reason: `ramdisk_find()` runs after CR3 switches.

```text
runelf hello arg1 arg2

Stack layout built before iret (top of user stack, growing down):
  "hello\0"          string data (copied from proc->user_arg_data, ring-3 accessible)
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
2. Restores keyboard routing to shell mode (`keyboard_set_process_mode(0)`)
3. Switches CR3 to `s_parent_proc->pd` if parent is a user process, or to the kernel PD if parent is the shell
4. Restores TSS ESP0 to `s_parent_esp0`
5. Saves a pointer to `proc->exit_ctx`
6. Calls `process_destroy(proc)` to free the process PD, user frames, kernel stack frame, and `process_t` frame
7. Calls `process_set_current(s_parent_proc)`
8. Executes `sti`
9. `longjmp`s back to the `setjmp` checkpoint in `elf_run_image()`

The explicit `sti` is required because `SYS_EXIT` enters through an interrupt gate, which clears IF. Since the exit path returns through `longjmp()` instead of unwinding back through `iretd`, the parent (or shell) would otherwise resume with interrupts disabled.

When the `longjmp` returns to `elf_run_image()`, that function returns to its caller (the shell command path or `sys_exec_impl`).

---

# Design Constraints

* No filesystem-backed program loading yet — user programs must live in the ramdisk at build time
* No dynamic linking — user programs are statically linked at `0x400000`
* `SYS_EXEC` is one-deep — `s_parent_proc` / `s_parent_esp0` are single statics, not a stack
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
        → elf_seed_sched_context()   copy argv into process_t
        → save s_parent_proc / s_parent_esp0
        → tss_set_kernel_stack()
        → process_set_current(proc)
        → setjmp(proc->exit_ctx)
        → keyboard_set_process_mode(1)
        → paging_switch(process_pd)
        → iret into ring 3
        → [program runs; syscalls use int 0x80]
        → sys_exit() → elf_process_exit()
            → paging_switch(parent->pd or kernel_pd)
            → tss_set_kernel_stack(s_parent_esp0)
            → process_destroy(proc)
            → process_set_current(s_parent_proc)
            → sti
            → longjmp() → return to parent

SYS_EXEC (from ring-3):
        → sys_exec_impl: copy name to kernel buffer
        → elf_run_named() [full runelf path above, nested]
        → child exits → longjmp → elf_run_image returns 1
        → sys_exec_impl returns 0 → regs->eax = 0
        → isr128_stub iretd → parent resumes in ring 3

SYS_YIELD (from ring-3):
        → sys_yield_impl((unsigned int)regs)
        → sched_yield_now(esp)
            → s_tick_count = 0
            → sched_do_switch(esp)
                → save current sched_esp = esp
                → pick next runnable slot
                → sched_switch(...)
        → [parent resumes here when scheduled next]
        → isr128_stub iretd → ring 3 resumes
```