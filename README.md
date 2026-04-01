# SimpleOS

SimpleOS is a BIOS-based x86 hobby operating system built with:

* `nasm`
* `i686-elf-gcc`
* `i686-elf-ld`
* `QEMU`

It boots from a raw disk image, switches to 32-bit protected mode, enables paging, loads a ramdisk from disk, and runs a C kernel with a terminal shell, ring-3 ELF program execution, a preemptive round-robin scheduler, voluntary yielding, and inter-process exec.

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
* Ramdisk — flat ELF archive loaded from disk, no kernel rebuild to add programs
* ELF loader — validates, loads segments, zeroes BSS, launches in ring 3
* `process_t` abstraction — per-process struct (PD, kernel stack, exit context, scheduler state, name, argv storage); fully PMM-allocated and reclaimed on exit
* Per-process page directories — address space isolation; PD itself freed on exit
* Ring 3 user mode — hardware-enforced privilege separation
* Syscall layer via `int 0x80` (DPL=3 gate): `SYS_WRITE`, `SYS_EXIT`, `SYS_GET_TICKS`, `SYS_PUTC`, `SYS_READ`, `SYS_YIELD`, `SYS_EXEC`
* `sys_exit()` returns cleanly to the parent caller (shell or user process) via `setjmp`/`longjmp`
* Shell now runs as an explicit kernel task scheduled by `scheduler.c`
* Per-process kernel stacks — dedicated PMM frame per process; TSS ESP0 set from it; freed on exit
* `SYS_READ` — blocking keyboard input for user programs; keyboard IRQ routed to ring buffer while process is running
* **Preemptive round-robin scheduler** — timer IRQ (100 Hz) context-switches between schedulable kernel contexts; 10-tick (100 ms) quantum
* **`SYS_YIELD`** — voluntary preemption; process surrenders its remaining quantum and the scheduler immediately switches to the next runnable task
* **`SYS_EXEC`** — a running user process can load and run a named child ELF from the ramdisk, block until it exits, and receive a return code; parent context (page directory, TSS ESP0, `process_t*`) is saved and fully restored

---

## Project Structure

```text
.
├── docs/           documentation
├── src/
│   ├── boot/       boot.asm, loader2.asm, kernel_entry.asm
│   ├── kernel/     kernel.c, gdt, idt, paging, memory, pmm, process,
│   │               scheduler, sched_switch.asm, syscall, timer, system, setjmp
│   ├── drivers/    keyboard, screen, terminal
│   ├── shell/      shell, line_editor, parse, commands
│   ├── exec/       elf_loader, exec, images, programs
│   └── user/       hello.c, ticks.c, args.c, readline.c, yield_test.c,
│                   exec_test.c, user_lib.h, user_syscall.h
├── tools/
│   └── mkramdisk.c
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

```
help
clear
echo [args...]
about
halt
reboot
uptime
meminfo

run <builtin>        call a kernel-linked program directly
runimg <image>       descriptor-based execution
runelf <n> [args]    load and run an ELF from the ramdisk
```

Current ramdisk programs: `hello`, `ticks`, `args`, `readline`, `yield_test`, `exec_test`

---

## Architecture Overview

```text
BIOS
 → boot.asm              load loader2 (CHS, 4 sectors)
 → loader2.asm           load kernel (LBA) + ramdisk (LBA) → protected mode
 → kernel_entry.asm      zero BSS → kernel_main()

kernel_main()
 → gdt_init()            GDT: null, k-code, k-data, u-code, u-data, TSS
 → paging_init()         enable paging, identity-map 8 MB
 → memory_init()         bump allocator at 0x100000
 → pmm_init()            bitmap allocator at 0x200000
 → keyboard/timer/idt    drivers and interrupt table
 → sched_init()          initialise runnable task table
 → ramdisk_init()        validate ramdisk at 0x10000
 → create shell task     explicit kernel task with its own stack
 → sti
 → sched_start(shell)    switch from boot stack into shell task

runelf hello
 → process_create()      allocate process_t from PMM
 → process_pd_create()   fresh page directory (PMM), kernel entries shared
 → map ELF + stack       pmm_alloc_frame() per page
 → alloc kernel stack    tss_set_kernel_stack(frame + PAGE_SIZE)
 → save parent context   s_parent_proc / s_parent_esp0
 → setjmp()              save kernel context for sys_exit return
 → paging_switch(pd)     load process CR3
 → iret into ring 3
 → [program runs in foreground; timer still ticks]
 → sys_exit() → elf_process_exit()
 → paging_switch(parent_pd or kernel_pd)
 → tss_set_kernel_stack(s_parent_esp0)
 → process_destroy()     free all PMM frames
 → process_set_current(s_parent_proc)
 → longjmp()             return to parent (shell or user process)

sys_exec("hello", ...)   [from inside a running user process]
 → copy name to kernel buffer
 → elf_run_named()       full runelf path above, nested
 → child exits → longjmp → iretd → parent resumes in ring 3
```

---

## Physical Memory Layout

```text
0x00007C00   bootloader stage 1
0x00008000   loader2 stage 2
0x00001000   kernel image
0x00006000   kernel .bss start (page tables + PMM bitmap)
0x0000A008   kernel .bss end (approx)
0x00010000   ramdisk (permanent)
0x00090000   shell static kernel stack top
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

This logic is implemented in `sched_proc_cr3()` and applies to any kernel task, regardless of its position in the scheduler table.

Today, the scheduler owns kernel tasks such as the shell. User ELF programs run through the foreground `setjmp`/`longjmp` path and are not yet enqueued as scheduler-owned tasks.

`SYS_YIELD` integrates with the scheduler via `sched_yield_now(esp)`. The `isr128_stub` frame layout is identical to `irq0_stub`, so the yield resume point is treated exactly like a timer preemption by `sched_switch`.

```text
irq0_stub       → pushes full register frame + ESP
irq0_handler_main(esp)
  timer_handle_irq()
  EOI             ← before sched_tick (critical ordering)
  sched_tick(esp)
    [every 10 ticks]
    save cur->sched_esp = esp
    pick next runnable entry (skip sched_esp==0 or state!=RUNNING)
    sched_switch(&cur->sched_esp, nxt->sched_esp, next_cr3, next_esp0)
      load all args into registers
      *save_esp = current ESP
      tss.esp0  = next_esp0
      CR3       = next_cr3
      ESP       = next_esp
      ret       → resumes incoming context
```

---

## Current Limitations

* No filesystem — programs must be in ramdisk at build time
* ELF programs linked at fixed address 0x400000 — no PIE/relocation
* `SYS_EXEC` is one-deep — `s_parent_proc` / `s_parent_esp0` are single statics; deeper nesting is not yet supported
* Kernel trusts user pointers in syscalls (no copy-from-user validation)
* ELF processes are not yet scheduler-owned tasks

---

## Direction

Next steps:

* Filesystem-backed storage (FAT12 or custom FS via ATA PIO)

---

## License

Personal / educational project.