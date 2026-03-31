# Architecture Overview

This document describes how SimpleOS boots, initializes, and executes programs.

---

# Boot Flow

```text
BIOS
  ↓
boot.asm (stage 1, 16-bit)
  ↓
loader2.asm (stage 2, 16-bit → 32-bit)
  loads kernel  → 0x1000
  loads ramdisk → 0x10000
  ↓
kernel_entry.asm (32-bit)
  zeros BSS
  ↓
kernel_main()
  gdt_init()        ← null, k-code, k-data, u-code, u-data, TSS + ltr
  paging_init()     ← identity-maps first 8 MB, enables CR0.PG
  memory_init()     ← bump allocator base at 0x100000
  pmm_init()        ← bitmap allocator at 0x200000–0x7FFFFF
  keyboard/timer/idt
  sched_init()      ← initialise runnable task table, wire tss_esp0_ptr
  ramdisk_init()
  create shell task ← explicit kernel task with dedicated stack
  sti
  sched_start()     ← switch from boot stack into shell task
```

---

## Stage 1 – boot.asm

* Loaded by BIOS at `0x7C00`
* Loads stage 2 via CHS `INT 0x13 AH=0x02` (4 sectors, fits within one track)
* Must be exactly **512 bytes**, ending with `dw 0xAA55`

---

## Stage 2 – loader2.asm

Runs in **real mode**, then switches to **protected mode**.

* Checks `INT 0x13 AH=0x41` for LBA extension support — halts if not available
* Loads kernel (LBA 5) to `0x1000` via `INT 0x13 AH=0x42`
* Loads ramdisk to `0x10000` (below 1MB real-mode limit)
* Sets up temporary GDT, enables protected mode, jumps to `0x1000`

Three values injected by Makefile at build time: `KERNEL_SECTORS`, `RAMDISK_SECTORS`, `RAMDISK_LBA`.

Loader2 GDT is temporary — kernel installs its own immediately.

---

## Kernel Entry – kernel_entry.asm

```asm
extern bss_start
extern bss_end

_start:
    mov edi, bss_start
    mov ecx, bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb            ; zero all BSS including page tables and PMM bitmap

    call kernel_main
```

BSS zeroing is mandatory. The three paging structures live in `.bss` at `0x7000`–`0x9FFF`. The PMM bitmap also lives in `.bss`. Without zeroing, `paging_init()` triple-faults and the PMM bitmap would falsely show all frames as used.

---

# Kernel Initialization

Inside `kernel_main()`:

1. `terminal_init()` — VGA text mode, cursor control
2. `gdt_init()` — install GDT with ring-3 segments and TSS; load task register with `ltr`
3. `paging_init()` — enable paging, identity-map 8 MB
4. `memory_init(0x100000)` — bump allocator starts at 1 MB
5. `pmm_init()` — bitmap allocator covers 0x200000–0x7FFFFF; all frames start free
6. `keyboard_init()`, `timer_init(100)`, `idt_init()` — drivers and interrupt table
7. `sched_init()` — initialise the scheduler data structures and wire the TSS ESP0 pointer for the switch helper
8. `ramdisk_init(0x10000)` — validate ramdisk magic, store pointer
9. `process_create_kernel_task("shell", shell_task_main)` — create the shell as an explicit kernel task
10. `sched_enqueue(shell_proc)` — make the shell runnable
11. `sti` — enable interrupts
12. `sched_start(shell_proc)` — switch from the boot stack into the shell task

`sched_init()` must still be called before `sti`, and `sched_start()` must happen only after the first runnable task has been created.

---

# GDT Layout

```text
Index 0   selector 0x00   null
Index 1   selector 0x08   kernel code   DPL=0   access=0x9A  gran=0xCF
Index 2   selector 0x10   kernel data   DPL=0   access=0x92  gran=0xCF
Index 3   selector 0x1B   user code     DPL=3   access=0xFA  gran=0xCF
Index 4   selector 0x23   user data     DPL=3   access=0xF2  gran=0xCF
Index 5   selector 0x28   TSS           DPL=0   32-bit available TSS
```

Selector values for ring-3 entries include RPL=3 in the low two bits (`0x18|3=0x1B`, `0x20|3=0x23`).

---

# Process Abstraction

Each `runelf` invocation still creates a `process_t` struct allocated from a single PMM frame. The same structure is now also used for kernel tasks such as the shell:

```c
typedef struct {
    u32*            pd;                     /* PMM-allocated page directory     */
    u32             kernel_stack_frame;     /* PMM frame for ring-0 syscall stack */
    jmp_buf         exit_ctx;               /* setjmp context for sys_exit return */
    unsigned int    sched_esp;              /* kernel ESP saved on preemption    */
    process_state_t state;                  /* UNUSED / RUNNING / EXITED        */
    char            name[32];               /* null-terminated process name      */
} process_t;
```

`sched_esp` is 0 until the process has been preempted at least once. The scheduler skips any slot where `state != RUNNING` or `sched_esp == 0`.

---

# Scheduler

A round-robin preemptive scheduler runs on every timer tick (100 Hz, quantum = 10 ticks = 100 ms).

## Process table

Fixed array of up to 8 `process_t*` slots. Slot 0 is always the shell/idle context — a static `process_t` with `pd = 0` as a sentinel meaning "use kernel PD".

## Context switch path

```text
timer fires (IRQ0)
  ↓
irq0_stub — pusha + segment pushes, then push esp, call irq0_handler_main
  ↓
irq0_handler_main(esp)
  timer_handle_irq()
  EOI sent here (before sched_tick — see ordering note below)
  sched_tick(esp)
    ↓
    [if quantum not expired] return
    ↓
    save esp → s_table[current]->sched_esp
    pick next slot (skip if sched_esp == 0 or state != RUNNING)
    sched_switch(&cur->sched_esp, nxt->sched_esp, next_cr3, next_esp0)
      load all args into registers
      *save_esp = current esp
      tss.esp0  = next_esp0
      cr3       = next_cr3
      esp       = next_esp
      ret  ←  resumes incoming context here
    ↓
    [outgoing context resumes here when switched back to]
  ↓
irq0_handler_main returns
  ↓
irq0_stub — add esp 4, pop segments, popa, iretd → ring 3 resumes
```

**EOI ordering**: EOI is sent before `sched_tick`. If `sched_switch` lands on a different context and the outgoing `irq0_handler_main` never returns through the normal path, the PIC is already unmasked and future timer ticks fire correctly on the new context. Sending EOI after `sched_tick` would permanently mask IRQ0 for the incoming context.

## Lifecycle

```text
runelf:
  process_create()
  sched_enqueue(proc)      ← added to run queue before iret
  setjmp / iret into ring 3

sys_exit:
  sched_dequeue(proc)      ← removed from run queue
  paging_switch(kernel_pd)
  tss_set_kernel_stack(0x90000)
  process_destroy(proc)
  longjmp → shell
```

---

# Physical Memory Layout

```text
0x00007C00   bootloader stage 1
0x00008000   loader2 stage 2 (done after protected-mode jump)
0x00001000   kernel image
0x00006000   kernel .bss start (page tables + PMM bitmap)
~0x0000A008  kernel .bss end
0x00010000   ramdisk (permanent)
0x00090000   kernel stack top (grows downward; shell context)
0x00100000   bump allocator base — permanent kernel structures
               kmalloc()      — long-lived kernel-owned data only
0x00200000   PMM base — reclaimable frames
               pmm_alloc_frame() — process_t structs, process PDs,
                                   ELF segment frames, user stack frames,
                                   all process-private page tables,
                                   per-process kernel stack frames
0x00800000   PMM ceiling (= identity-map limit)
0x00400000   USER_CODE_BASE — user ELF virtual address (per-process mapping)
0xBFFFF000   user stack virtual address (per-process mapping)
```

---

# Virtual Address Layout Per Process

```text
0x000000 – 0x3FFFFF   kernel (shared, supervisor-only — inaccessible from ring 3)
0x400000 – 0x7FFFFF   user ELF segments (private, PAGE_USER | PAGE_WRITE)
0xBFFFF000            user stack page (private, PAGE_USER | PAGE_WRITE)
PD 2–1023 range       kernel heap etc. (shared, supervisor-only)
```

## Process Paging Ownership

User processes own their paging structures:

* Page directory — PMM-allocated, freed on process exit
* User-space page tables — PMM-allocated, freed on process exit
* User-space frames (ELF, stack) — PMM-allocated, freed on process exit

Kernel mappings remain shared from `kernel_page_directory` and are never reclaimed by per-process teardown.

`process_pd_destroy()` walks the user PDE range, frees all mapped user frames, frees each private user page table, then frees the page directory frame itself.

---

# Ramdisk

Flat binary archive (`ramdisk.rd`) loaded to `0x10000` by loader2.

Format: `rd_header_t` (magic + count) + `rd_entry_t[]` (name, offset, size) + raw ELF data.

Built by `tools/mkramdisk` (C tool, compiled with host gcc).

`ramdisk_find(name)` returns a pointer directly into the ramdisk image — no copy made.

Current programs: `hello`, `ticks`, `args`, `readline` — all linked at `0x400000` with `-Ttext-segment`.

---

# ELF Execution Model

```text
runelf hello arg1
  ↓
ramdisk_find("hello")
  ↓
elf_run_image(data, argc, argv)
  ↓
validate ELF magic
process_create("elf")           → allocate process_t from PMM
process_pd_create()             → fresh PD (pmm_alloc_frame), kernel entries shared
map ELF segments                → pmm_alloc_frame() per page, PAGE_USER at 0x400000
map user stack                  → pmm_alloc_frame(), PAGE_USER at 0xBFFFF000
alloc kernel stack              → pmm_alloc_frame(), tss_set_kernel_stack(frame + PAGE_SIZE)
process_set_current(proc)
proc->state = RUNNING
sched_enqueue(proc)             → added to scheduler run queue
setjmp(proc->exit_ctx)          → save kernel context for sys_exit return
keyboard_set_process_mode(1)
paging_switch(proc->pd)
iret → ring 3
  ↓
[program runs at CPL=3; timer preempts it every ~100 ms]
  ↓
sys_exit() → int 0x80 → elf_process_exit()
  proc->state = EXITED
  keyboard_set_process_mode(0)
  sched_dequeue(proc)
  paging_switch(kernel_pd)
  tss_set_kernel_stack(0x90000)
  process_destroy(proc)
  process_set_current(0)
  longjmp(exit_ctx, 1)          → resume in elf_run_image → return to shell
```

---

# Interrupt Architecture

## IDT entries

```text
8    → ISR8 (double fault — VGA marker '8' white-on-red at row 1 col 12 + halt)
32   → IRQ0 (timer, DPL=0)
33   → IRQ1 (keyboard, DPL=0)
128  → syscall int 0x80 (DPL=3 — callable from ring 3)
```

## IRQ0 flow (with scheduler)

```text
timer fires
  ↓
irq0_stub: pusha, push segments, push esp, call irq0_handler_main(esp)
  ↓
irq0_handler_main:
  timer_handle_irq()
  EOI (outb 0x20, 0x20)        ← before sched_tick
  sched_tick(esp)
    [may call sched_switch and not return to this invocation]
  ↓
irq0_stub: add esp 4, pop segments, popa, iretd
```

## IRQ1 flow

```text
irq1_stub: pusha, push segments
  ↓
irq1_handler_main:
  EOI (outb 0x20, 0x20)        ← sent before handler (consistent with IRQ0 pattern)
  keyboard_handle_irq()
  ↓
irq1_stub: pop segments, popa, iretd
```

## Syscall flow (ring 3 → ring 0 → ring 3)

```text
int 0x80 (CPL=3)
  ↓
CPU: load SS0/ESP0 from TSS — switch to per-process kernel stack
CPU: push SS, ESP, EFLAGS, CS, EIP
  ↓
isr128_stub: pusha, push segments, push esp, call syscall_handler_main(regs)
  ↓
regs->eax = result
  ↓
pop segments, popa, iretd → ring 3 resumes
```

---

# Syscall Architecture

## Register ABI

```text
eax = syscall number
ebx = arg1
ecx = arg2
edx = arg3
return → eax
```

---

# Terminal + Shell

## Terminal

VGA text mode (`0xB8000`). Provides `terminal_putc`, `terminal_puts`, `terminal_put_uint`, `terminal_put_hex`, cursor control, scrolling.

## Shell

```text
keyboard IRQ → keyboard_handle_irq()
  ↓
  if kb_process_mode: push to ring buffer (for SYS_READ)
  else: shell_input_char()
    ↓
    line_editor (insert, delete, cursor, history)
    ↓
    [Enter] → parse_command() → commands_execute()
    ↓
    run / runimg / runelf / meminfo / ... dispatch
```

---

# User Programs

Entry point convention:

```c
void _start(int argc, char** argv)
```

All user programs must be linked at `USER_CODE_BASE` (0x400000) using `-Ttext-segment`.

I/O via syscalls. No libc, no runtime. Must call `sys_exit(0)` before returning.

Current ramdisk programs: `hello`, `ticks`, `args`, `readline`.

---

# Build System

## Disk image layout

```text
LBA 0         boot.bin         (1 sector)
LBA 1–4       loader2.bin      (4 sectors)
LBA 5+        kernel.bin       (padded to 512-byte boundary)
after kernel  ramdisk.rd
```

## Key generated artifacts

```text
build/gen/loader2.gen.asm    KERNEL_SECTORS / RAMDISK_SECTORS / RAMDISK_LBA injected
build/bin/kernel_padded.bin  kernel.bin padded to sector boundary
build/bin/ramdisk.rd         packed by build/tools/mkramdisk
build/obj/setjmp.o           assembled from src/kernel/setjmp.asm
build/obj/sched_switch.o     assembled from src/kernel/sched_switch.asm
```

---

# Known Limitations

* ELF link address fixed at 0x400000 — no PIE/relocation support
* No filesystem — programs must be in ramdisk at build time
* No `sched_yield` syscall — voluntary preemption not yet possible

---

# Future Architecture Direction

1. Filesystem-backed ELF loading — FAT12 or custom FS via ATA PIO
2. `SYS_YIELD` / `SYS_EXEC` syscalls

---

# Debugging Map

| Stage              | Tool                                                    |
| ------------------ | ------------------------------------------------------- |
| boot (real mode)   | BIOS print (int 0x10)                                   |
| early kernel       | VGA direct (0xB8000)                                    |
| kernel             | terminal_puts                                           |
| user process       | sys_write / sys_putc / sys_read                         |
| memory accounting  | meminfo command                                         |
| crash analysis     | QEMU -d int,cpu_reset,guest_errors -D qemu.log          |

---

# Summary

SimpleOS is currently:

```text
two-stage bootloader (CHS + LBA)
paging enabled (identity-mapped 8 MB)
GDT with ring-3 user segments and TSS
process_t abstraction — per-process struct (PD, kernel stack, exit ctx, sched_esp, name)
per-process page directories — address space isolation, fully reclaimed on exit
ring-3 ELF execution (hardware privilege enforcement)
int 0x80 syscall interface (DPL=3 gate, TSS stack switch)
argv copied into user-accessible stack memory
clean process exit via setjmp/longjmp
physical memory manager (bitmap, all frames reclaimed on exit — no leak)
per-process kernel stacks (PMM frame per process, freed on exit)
SYS_READ — blocking keyboard input for user programs
preemptive round-robin scheduler — timer IRQ context switch, 100 ms quantum
interactive shell with meminfo command
```

Foundation for:

* filesystem access
* SYS_YIELD / SYS_EXEC