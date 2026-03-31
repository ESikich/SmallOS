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
  sti
  ramdisk_init()
  shell_init()
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
7. `sti` — enable interrupts
8. `ramdisk_init(0x10000)` — validate ramdisk magic, store pointer
9. `shell_init()` — start interactive shell

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

## TSS

The TSS is a `tss_t` struct in `gdt.c`. Only two fields matter for the current design:

```text
ss0  = 0x10   kernel data segment for ring-3→ring-0 stack switch
esp0 = set per-process by tss_set_kernel_stack() before each iret
```

`esp0` is set to `kernel_stack_frame + PAGE_SIZE` — the top of a dedicated 4 KB PMM frame
allocated per process. This frame is freed by `elf_process_exit()` on process exit.

The task register is loaded once at boot with `ltr 0x28`. `tss_set_kernel_stack()` updates `tss.esp0` in place — the TR does not need to be reloaded.

---

# Paging Architecture

## Kernel page directory

Three static arrays in `.bss`, zeroed at boot:

```text
kernel_page_directory[1024]   master PD
low_page_table_0[1024]        PD index 0 → 0x000000–0x3FFFFF (identity, supervisor)
low_page_table_1[1024]        PD index 1 → 0x400000–0x7FFFFF (identity, supervisor)
```

After `paging_init()`: virtual == physical for all addresses 0x000000–0x7FFFFF.

## Per-process page directories

Each `runelf` invocation creates a private page directory:

```text
process_pd_create()
  → allocate 4KB-aligned PD via kmalloc_page()
  → copy kernel PD entries (indices 0 and 2–1023) — shared kernel access
  → leave PD index 1 empty — user ELF region, private per process
```

The user ELF is mapped at `USER_CODE_BASE` (0x400000) with `PAGE_USER | PAGE_WRITE` using frames from `pmm_alloc_frame()`. A user stack page is mapped at `USER_STACK_TOP - PAGE_SIZE` (0xBFFFF000), also from `pmm_alloc_frame()`.

Kernel entries are inherited but **supervisor-only** — ring-3 code cannot access them.

## Physical memory layout for allocators

```text
0x100000 – 0x1FFFFF   bump allocator (kmalloc / kmalloc_page)
                       process PDs, page tables, argv, parse buffers
0x200000 – 0x7FFFFF   PMM (pmm_alloc_frame / pmm_free_frame)
                       user ELF frames, user stack frames, ELF page table,
                       per-process kernel stack frames
```

The ranges are disjoint. Both are identity-mapped so physical address == virtual address.

## Virtual address layout per process

```text
0x000000 – 0x3FFFFF   kernel (shared, supervisor-only — inaccessible from ring 3)
0x400000 – 0x7FFFFF   user ELF segments (private, PAGE_USER | PAGE_WRITE)
0xBFFFF000            user stack page (private, PAGE_USER | PAGE_WRITE)
PD 2–1023 range       kernel heap etc. (shared, supervisor-only)
```

---

# Memory Layout

```text
0x00007C00   bootloader stage 1
0x00008000   loader2 stage 2 (done after jump to kernel)
0x00001000   kernel image
0x00006000   kernel .bss start (page tables + PMM bitmap here)
~0x0000A008  kernel .bss end
0x00010000   ramdisk (permanent)
0x00090000   kernel stack top (grows downward)
0x00100000   bump allocator base → grows upward to 0x1FFFFF
               kmalloc()      — argv arrays, parse buffers
               kmalloc_page() — process PDs, page tables (non-ELF PDEs)
0x00200000   PMM base → reclaimed frames reused
               pmm_alloc_frame() — ELF frames, stack frames, ELF PDE page table,
                                   per-process kernel stack frames
0x00800000   PMM + identity-map ceiling
0x00400000   USER_CODE_BASE — user ELF virtual address (per-process mapping)
0xBFFFF000   user stack virtual address (per-process mapping)
```

---

# Ramdisk

Flat binary archive (`ramdisk.rd`) loaded to `0x10000` by loader2.

Format: `rd_header_t` (magic + count) + `rd_entry_t[]` (name, offset, size) + raw ELF data.

Built by `tools/mkramdisk` (C tool, compiled with host gcc).

`ramdisk_find(name)` returns a pointer directly into the ramdisk image — no copy made.

Current programs: `hello`, `ticks` — both linked at `0x400000` with `-Ttext-segment`.

---

# ELF Execution Model

```text
runelf hello arg1
  ↓
ramdisk_find("hello")           → pointer into ramdisk at 0x10000
  ↓
elf_run_image(data, argc, argv)
  ↓
validate ELF magic
  ↓
process_pd_create()             → fresh PD (kmalloc_page), kernel entries shared
  ↓
for each PT_LOAD segment:
    pages = ceil(p_memsz / 4096)
    for each page:
        frame = pmm_alloc_frame()   ← from 0x200000–0x7FFFFF
        mem_zero(frame, 4096)
        paging_map_page(pd, p_vaddr + p*4096, frame, PAGE_USER | PAGE_WRITE)
    copy p_filesz bytes from ramdisk into frames via physical addresses
  ↓
stack_frame = pmm_alloc_frame()
paging_map_page(pd, 0xBFFFF000, stack_frame, PAGE_USER | PAGE_WRITE)
  ↓
kernel_stack_frame = pmm_alloc_frame()
tss_set_kernel_stack(kernel_stack_frame + PAGE_SIZE)  → TSS ESP0 for ring-3 syscalls
  ↓
setjmp(s_exit_ctx)              → save kernel stack context for sys_exit return
  ↓
paging_switch(process_pd)       → load process CR3, flush TLB
  ↓
elf_enter_ring3()
  copy argv to user stack
  build cdecl frame
  iret → ring 3, EIP=e_entry
  ↓
[program runs at CPL=3]
  ↓
sys_exit() → int 0x80 → elf_process_exit()
  paging_switch(kernel_pd)
  process_pd_destroy(pd)        → walk all private PDEs, pmm_free_frame each frame
  pmm_free_frame(kernel_stack_frame)
  longjmp(s_exit_ctx, 1)        → resume in elf_run_image
  ↓
return to shell
```

---

# Interrupt Architecture

## IDT entries

```text
8    → ISR8 (double fault — VGA marker '8' in red + halt)
32   → IRQ0 (timer, DPL=0)
33   → IRQ1 (keyboard, DPL=0)
128  → syscall int 0x80 (DPL=3 — callable from ring 3)
```

## IRQ flow

```text
hardware interrupt
  ↓
irqX_stub (saves registers, loads kernel segments 0x10)
  ↓
irqX_handler_main (C)
  ↓
driver logic + EOI (outb 0x20, 0x20)
  ↓
iretd
```

## Syscall flow (ring 3 → ring 0 → ring 3)

```text
int 0x80 (CPL=3)
  ↓
CPU: check IDT[128].DPL=3 — OK
CPU: load SS0/ESP0 from TSS — switch to kernel stack
CPU: push SS, ESP, EFLAGS, CS, EIP
  ↓
isr128_stub: pusha, push segments, load kernel DS=0x10
  ↓
syscall_handler_main(regs)
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

## Stack frame contract

`isr128_stub` pushes: `pusha` then `ds/es/fs/gs`. The struct pointer passed to C points to `gs` (last pushed = lowest address = first field). See `syscalls.md` for the full layout.

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

All user programs must be linked at `USER_CODE_BASE` (0x400000) using `-Ttext-segment`:

```bash
i686-elf-ld -m elf_i386 -Ttext-segment 0x400000 -e _start prog.o -o prog.elf
```

Use `-Ttext-segment`, not `-Ttext`. `-Ttext` causes the linker to insert a header segment
at `0x3FF000` (PD index 0), which shares the kernel page table and leaks a PMM frame per run.

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
```

---

# Known Limitations

* `process_pd_destroy` does not free the PD itself — it came from `kmalloc_page` (bump allocator, no free). One 4 KB heap leak per `runelf` invocation.
* Page tables for non-ELF PDEs (e.g. the stack's PD index 767) come from `kmalloc_page` and are not freed per-process.
* No scheduler — shell blocks during program execution
* ELF link address fixed at 0x400000 — no PIE/relocation support
* No filesystem — programs must be in ramdisk at build time

---

# Future Architecture Direction

1. Process abstraction — `process_t` struct: saved register state, kernel stack, PD pointer, name
2. Preemptive scheduler — timer IRQ context switch using per-process kernel stacks
3. Filesystem-backed ELF loading — FAT12 or custom FS via ATA PIO
4. Free process PDs — move PD allocation to PMM so `process_pd_destroy` can free everything

---

# Debugging Map

| Stage              | Tool                           |
| ------------------ | ------------------------------ |
| boot (real mode)   | BIOS print (int 0x10)          |
| early kernel       | VGA direct (0xB8000)           |
| kernel             | terminal_puts                  |
| user process       | sys_write / sys_putc / sys_read |
| memory accounting  | meminfo command                |
| crash analysis     | QEMU -d int,cpu_reset,guest_errors -D qemu.log |

---

# Summary

SimpleOS is currently:

```text
two-stage bootloader (CHS + LBA)
paging enabled (identity-mapped 8 MB)
GDT with ring-3 user segments and TSS
per-process page directories (address space isolation)
ring-3 ELF execution (hardware privilege enforcement)
int 0x80 syscall interface (DPL=3 gate, TSS stack switch)
argv copied into user-accessible stack memory
clean process exit via setjmp/longjmp
physical memory manager (bitmap, frames reclaimed on exit)
per-process kernel stacks (PMM frame per process, freed on exit)
SYS_READ — blocking keyboard input for user programs
interactive shell with meminfo command
```

Foundation for:

* process abstraction (`process_t`) and scheduling
* filesystem access