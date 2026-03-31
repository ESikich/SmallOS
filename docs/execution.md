# Execution Model

This document defines how programs are invoked, loaded, and executed in the system.

---

# Overview

Programs can be executed through three paths:

```text
run      → built-in C functions (kernel-linked, direct call, ring 0)
runimg   → descriptor-based image execution (ring 0)
runelf   → ELF programs loaded from ramdisk into per-process address space (ring 3)
```

`runelf` is the primary execution path. Programs run in ring 3 with hardware-enforced memory protection. The shell blocks for the duration of each program. Timer interrupts continue to fire during execution (tick counter advances), but keyboard input is routed to the process input ring buffer rather than the shell while a process is running.

---

# Command Flow

```text
keyboard input
  ↓
shell_input_char()
  ↓
line_editor (insert, delete, cursor movement, history)
  ↓
[Enter pressed]
  ↓
parse_command()     builds argc / argv from input string via kmalloc
  ↓
commands_execute()  linear search of command table
  ↓
dispatch to handler
```

---

# Execution Types

## 1. Built-in Programs (`run`)

Direct function pointer call. No page directory change. Runs in ring 0 in the kernel's address space. Use case: simple utilities, debugging.

## 2. Image Programs (`runimg`)

Descriptor table of function pointers. Transitional — superseded by ELF execution.

## 3. ELF Programs (`runelf`)

The full execution sequence:

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
process_set_current(proc)        → register as active process
proc->state = RUNNING
  ↓
setjmp(proc->exit_ctx)           → save kernel stack context for sys_exit return
  ↓
keyboard_set_process_mode(1)     → route keystrokes to ring buffer
paging_switch(pd)                → load process CR3, flush TLB
  ↓
elf_enter_ring3(entry, USER_STACK_TOP, argc, argv)
  → copy argv strings onto user stack (ring-3 accessible)
  → build argv pointer array on user stack
  → build cdecl call frame: [ret=0, argc, argv]
  → load DS/ES/FS/GS = SEG_USER_DATA (0x23)
  → push iret frame: SS=0x23, ESP, EFLAGS, CS=0x1B, EIP=e_entry
  → iret  → CPU switches to ring 3
  ↓
[program runs in ring 3, uses int 0x80 for syscalls]
  ↓
sys_exit(code) → int 0x80 → syscall_handler_main → elf_process_exit()
  ↓
elf_process_exit():
  proc->state = EXITED
  keyboard_set_process_mode(0)
  paging_switch(kernel_pd)       → restore kernel CR3
  save &proc->exit_ctx           → copy pointer before freeing proc frame
  process_destroy(proc)          → process_pd_destroy (frees ELF frames + PD)
                                    pmm_free_frame(kernel_stack_frame)
                                    pmm_free_frame(process_t frame)
  process_set_current(0)
  longjmp(exit_ctx, 1)           → resume in elf_run_image after setjmp
  ↓
elf_run_image() returns 1        → shell prompt reappears
```

---

# Ring 3 Privilege Model

ELF programs execute at CPL=3. The hardware enforces the following:

* Kernel pages (no `PAGE_USER` flag) are inaccessible to ring-3 code — any attempt causes a #GP
* User pages (`PAGE_USER | PAGE_WRITE`) at 0x400000+ and 0xBFFFF000 are readable and writable
* `int 0x80` is reachable from ring 3 because the IDT gate has DPL=3 (`IDT_FLAG_INT_GATE_USER`)
* On `int 0x80`, the CPU reads TSS.SS0 and TSS.ESP0 to find the ring-0 stack — set by `tss_set_kernel_stack()` before each launch

---

# Argument Passing

`parse_command()` allocates `argv` strings from the kernel heap. Before `iret`, `elf_enter_ring3()` copies all argv strings and the pointer array onto the user stack so they are accessible from ring 3:

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
3. CPU loads SS0/ESP0 from TSS — switches to the per-process kernel stack
4. CPU pushes SS, ESP, EFLAGS, CS, EIP onto kernel stack
5. `isr128_stub` saves registers, loads kernel segments (0x10), calls `syscall_handler_main`
6. Handler executes, sets return value in `regs->eax`
7. `iretd` returns to ring-3 code

Kernel data remains accessible during syscalls because kernel PD entries are shared into every process directory (supervisor-only — ring-3 code itself cannot access them).

---

# Keyboard Input During Execution

When a process is running, `keyboard_set_process_mode(1)` redirects all ASCII keystrokes from the shell into a 256-byte ring buffer. `SYS_READ` (syscall 5) drains this buffer with a blocking STI+HLT loop. When the process exits, `keyboard_set_process_mode(0)` restores shell routing and clears the buffer.

The syscall gate is an interrupt gate — IF is cleared on entry. `sys_read_impl` re-enables interrupts with `sti` before the wait loop and restores `cli` before returning, so keyboard IRQs can fire and populate the buffer during blocking reads.

---

# Address Space During Execution

```text
process PD (active during ring-3 execution):
  0x000000–0x3FFFFF   shared from kernel PD (supervisor — ring 3 cannot access)
  0x400000+           user ELF pages (PAGE_USER | PAGE_WRITE)
  0xBFFFF000          user stack page (PAGE_USER | PAGE_WRITE)
  PD indices 2–1023   shared from kernel PD (heap, etc. — supervisor)
```

---

# Exit Path

`sys_exit()` is the only valid way for a ring-3 program to terminate. The `_start` entry point should always call `sys_exit(0)` before returning; a bare `ret` from `_start` has no valid return address and will fault.

`elf_process_exit()` runs in ring 0 (inside the syscall handler):

1. Sets `proc->state = EXITED` and restores keyboard to shell mode
2. Switches CR3 back to the kernel page directory
3. Saves a pointer to `proc->exit_ctx` (the jmp_buf is inside the process_t frame)
4. Calls `process_destroy(proc)` — frees ELF frames, user stack frame, all process-private page tables, the PD frame, the kernel stack frame, and the process_t frame itself
5. Calls `longjmp` via the saved pointer — resumes `elf_run_image()` after the `setjmp` checkpoint
6. `elf_run_image()` returns 1 to the shell command handler

The longjmp-after-free is safe because `longjmp` copies the jmp_buf contents to the stack before the frame is freed, and the PMM frame is not reused until the next `pmm_alloc_frame()` call — which cannot happen during the longjmp itself.

---

# Design Constraints

* No scheduler — shell blocks during execution
* No dynamic linking — programs are statically linked at 0x400000
* No filesystem access from user programs — I/O via syscalls only

---

# Future Execution Model

## Scheduler

Preemptive: timer IRQ saves current process state into the active `process_t`, picks the next runnable process, and restores its state. `process_t` is already in place — the main work is adding a register save area and a process table.

## Dynamic Link Addresses

Position-independent ELF (`-pie`) removes the fixed `0x400000` requirement.

---

# Summary

```text
runelf  → ramdisk_find()
        → process_create()
        → process_pd_create()       ← PD from PMM
        → map ELF frames at 0x400000 (PAGE_USER)
        → map stack at 0xBFFFF000 (PAGE_USER)
        → tss_set_kernel_stack      (ESP0 for ring-3 syscalls)
        → process_set_current(proc)
        → setjmp(proc->exit_ctx)    (save kernel context)
        → keyboard_set_process_mode(1)
        → paging_switch(process_pd)
        → iret into ring 3
        → [program runs, int 0x80 syscalls, SYS_READ blocks on keyboard]
        → sys_exit → elf_process_exit
        → paging_switch(kernel_pd)
        → process_destroy(proc)     ← frees everything including PD + process_t
        → longjmp → return to shell
```