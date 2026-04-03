# SmallOS

SmallOS is a BIOS-based x86 hobby operating system built with:

* `nasm`
* `i686-elf-gcc`
* `i686-elf-ld`
* `QEMU`

It boots from a raw disk image, switches to 32-bit protected mode, enables paging, loads user programs from a FAT16 partition on disk, and runs a C kernel with a terminal shell, ring-3 ELF program execution, a preemptive round-robin scheduler, voluntary yielding, inter-process exec, and ATA PIO disk access.

---

## Current Features

* Two-stage BIOS bootloader (real mode → protected mode)
* LBA extended disk reads (INT 0x13 AH=0x42) — no CHS track limit
* Kernel-owned GDT with ring-3 user segments and TSS
* x86 paging — identity-mapped first 8 MB
* BSS zeroing in kernel entry before paging is enabled
* IDT with PIT timer (IRQ0), keyboard (IRQ1), and syscalls (INT 0x80)
* VGA text-mode display and terminal abstraction
* Shell with line editing, history, and command parsing
* Shell input processing decoupled from IRQ1 via a small event queue
* Bump allocator (`kmalloc`) for permanent kernel structures
* Physical memory manager (`pmm`) — bitmap allocator for reclaimable frames
  * Manages `0x200000`–`0x7FFFFF` (6 MB, 1536 frames)
  * All process frames reclaimed on exit — no leak after `runelf`
* ELF loader — validates, loads segments, zeroes BSS, launches in ring 3
* `process_t` abstraction — per-process struct (PD, kernel stack, exit context, scheduler state, argv storage, name); fully PMM-allocated and reclaimed on exit
* Per-process page directories — address space isolation; PD itself freed on exit
* Ring 3 user mode — hardware-enforced privilege separation
* Per-process kernel stacks — dedicated PMM frame per process; TSS ESP0 set from it; freed on exit
* Syscall layer via `int 0x80` (DPL=3 gate): `SYS_WRITE`, `SYS_EXIT`, `SYS_GET_TICKS`, `SYS_PUTC`, `SYS_READ`, `SYS_YIELD`, `SYS_EXEC`
* `sys_exit()` returns cleanly to the parent (shell or user process) via `setjmp`/`longjmp`
* Shell now runs as an explicit kernel task scheduled by `scheduler.c`
* **Preemptive round-robin scheduler** — timer IRQ (100 Hz) context-switches between kernel tasks; 10-tick (100 ms) quantum
* **`SYS_YIELD`** — voluntary preemption; process surrenders its remaining quantum immediately
**`SYS_EXEC`** — user process launches a named child ELF through the current foreground path, blocks until it exits, and returns `0` on success / `-1` on failure; the calling foreground context is saved/restored for this path, with only one explicit parent context tracked
* **ATA PIO driver** — polls the primary IDE channel (`0x1F0`) to read 512-byte sectors from disk in 32-bit protected mode; no DMA or IRQ required
* **FAT16 partition** — 16 MB FAT16 volume appended to the disk image containing all user ELFs; built by `tools/mkfat16.c` with no external dependencies; readable at runtime via ATA PIO

---

## Project Structure

```text
.
├── docs/           documentation
├── src/
│   ├── boot/       boot.asm, loader2.asm, kernel_entry.asm
│   ├── kernel/     kernel.c, gdt, idt, paging, memory, pmm, process,
│   │               scheduler, sched_switch.asm, syscall, timer, system, setjmp
│   ├── drivers/    keyboard, screen, terminal, ata, fat16
│   ├── shell/      shell, line_editor, parse, commands
│   ├── exec/       elf_loader
│   └── user/       hello.c, ticks.c, args.c, readline.c, exec_test.c,
│                   user_lib.h, user_syscall.h
├── tools/
│   └── mkfat16.c
├── build/
├── Makefile
├── linker.ld
```

---

## Build & Run

```bash
make clean && make
qemu-system-i386 -drive format=raw,file=build/img/os-image.bin
```

Use `-drive format=raw` (hard disk mode). Do not use `-fda` (floppy).

---

## Commands

```text
help
clear
echo [args...]
about
halt
reboot
uptime
meminfo
ataread <lba>        dump first 32 bytes of a disk sector (ATA PIO)

fsls               list FAT16 root directory
fsread <name>      dump first 16 bytes of a FAT16 file
runelf <name> [args] load and run an ELF from the FAT16 partition
```

Current FAT16 programs: `hello`, `ticks`, `args`, `runelf_test`, `readline`, `exec_test`

---

## Architecture Overview

```text
BIOS
 → boot.asm              load loader2 (CHS, 4 sectors)
 → loader2.asm           load kernel (LBA) → protected mode
 → kernel_entry.asm      zero BSS → kernel_main()

kernel_main()
 → terminal_init()       VGA text mode / terminal output
 → gdt_init()            GDT: null, k-code, k-data, u-code, u-data, TSS
 → paging_init()         enable paging, identity-map 8 MB
 → memory_init()         bump allocator at 0x100000
 → pmm_init()            bitmap allocator at 0x200000
 → keyboard/timer/idt    drivers and interrupt table
 → sched_init()          initialise runnable task table
 → ata_init()            initialise ATA primary channel
 → fat16_init()          read BPB, validate FAT16 volume
 → create shell task     explicit kernel task with its own stack
 → sti
 → sched_start(shell)    switch from boot stack into shell task

runelf hello
 → process_create()      allocate process_t from PMM
 → process_pd_create()   fresh page directory (PMM), kernel entries shared
 → map ELF + stack       pmm_alloc_frame() per page
 → alloc kernel stack    tss_set_kernel_stack(frame + PAGE_SIZE)
 → seed sched context    elf_seed_sched_context() builds proc->sched_esp for elf_user_task_bootstrap()
 → save parent context   s_parent_proc / s_parent_esp0
 → setjmp()              save kernel context for sys_exit return
 → paging_switch(pd)     load process CR3
 → iret into ring 3
 → [program runs in foreground; timer still ticks]
 → sys_exit() → elf_process_exit()
 → paging_switch(parent_pd or kernel_pd)
 → process_destroy()     free all PMM frames
 → longjmp()             return to parent
```

---

## Disk Image Layout

```text
LBA 0         boot.bin              (512 bytes)
LBA 1–4       loader2.bin           (2048 bytes)
LBA 5+        kernel_padded.bin     (sector-aligned)
LBA 5+ks      fat16.img             (16 MB FAT16 partition)
```

FAT16_LBA is patched into boot sector offset 504 after image assembly.

---

## Physical Memory Layout

```text
0x00007C00   bootloader stage 1
0x0000A000   loader2 stage 2
0x00001000   kernel image
0x00006000   kernel .bss start (page tables + PMM bitmap)
~0x0000A000  kernel .bss end (approx)
0x00090000   kernel stack top (boot / shell context)
0x00100000   bump allocator — permanent kernel structures
0x00200000   PMM — reclaimable frames
               process_t structs, process PDs, ELF frames,
               user stack frames, all process-private PTs, kernel stack frames
0x00800000   PMM ceiling (= identity-map limit)
0x00400000   USER_CODE_BASE (per-process ELF mapping)
0xBFFFF000   user stack (per-process mapping)
```

---

## Scheduler

Round-robin, preemptive, timer-driven at 100 Hz with a 10-tick (100 ms) quantum.

The scheduler uses a fixed-capacity table:

    s_table[SCHED_MAX_PROCS]

This table is a **plain dynamic array**, not a structured layout:

- There is **no special slot 0**
- There is **no sentinel process_t**
- Entries are appended by `sched_enqueue()` and compacted by `sched_dequeue()`
- `sched_start()` searches for the requested process instead of assuming a fixed position

The `pd == 0` rule still exists, but it is **not a table-layout concept**. It is interpreted at runtime:

- `pd == 0` → use kernel page directory
- otherwise → use process page directory

Today, the scheduler owns kernel tasks such as the shell. User ELF programs still run through the special foreground `setjmp`/`longjmp` path and are not yet scheduler-owned tasks.

---

## Context Switch Path

```text
timer interrupt
  ↓
irq0_stub
  ↓
irq0_handler_main(esp)
  ↓
sched_tick(esp)
  ↓
sched_switch(&cur->sched_esp, nxt->sched_esp, next_cr3, next_esp0)
  ↓
tss_set_kernel_stack(next_esp0)
  ↓
CR3 switch
  ↓
ESP switch
  ↓
ret → resumed context
```

This path no longer relies on a cached pointer into the packed TSS structure.

---

## Notes

* Launch as a hard disk, not a floppy
* User programs are linked at `0x400000`
* `fat16_load()` uses a static load buffer, so repeated `runelf` calls do not grow the heap
* The scheduler switch path updates TSS.ESP0 through `tss_set_kernel_stack()`, not by caching a pointer into the TSS

---

## Current Direction

The system is in a deliberate hybrid stage:

* the **shell** is scheduler-owned
* **ELF user programs** still execute through the foreground `setjmp` / `longjmp` path
* `elf_run_image()` already seeds a valid scheduler entry context for user ELFs via `elf_seed_sched_context()`

The next architectural step is to make `runelf` enqueue those user tasks so the scheduler owns first entry and full lifecycle.