# SimpleOS

SimpleOS is a BIOS-based x86 hobby operating system built with:

* `nasm`
* `i686-elf-gcc`
* `i686-elf-ld`
* `QEMU`

It boots from a raw disk image, switches to 32-bit protected mode, enables paging, loads a ramdisk from disk, and runs a C kernel with a terminal shell and ELF program execution in ring 3.

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
* Bump allocator (`kmalloc`) for kernel structures
* Physical memory manager (`pmm`) — bitmap allocator for user page frames
  * Manages `0x200000`–`0x7FFFFF` (6 MB, 1536 frames)
  * Frames reclaimed on process exit — no leak after `runelf`
* Ramdisk — flat ELF archive loaded from disk, no kernel rebuild to add programs
* ELF loader — validates, loads segments, zeroes BSS, launches in ring 3
* Per-process page directories — address space isolation per `runelf` invocation
* Ring 3 user mode — hardware-enforced privilege separation
* Syscall layer via `int 0x80` (DPL=3 gate): `SYS_WRITE`, `SYS_EXIT`, `SYS_GET_TICKS`, `SYS_PUTC`, `SYS_READ`
* `sys_exit()` returns cleanly to the shell via `setjmp`/`longjmp`
* Per-process kernel stacks — dedicated PMM frame per process; TSS ESP0 set from it; freed on exit
* `SYS_READ` — blocking keyboard input for user programs; keyboard IRQ routed to ring buffer while process is running

---

## Project Structure

```text
.
├── src/
│   ├── boot/       boot.asm, loader2.asm, kernel_entry.asm
│   ├── kernel/     kernel.c, gdt, idt, paging, memory, pmm, syscall, timer, system, setjmp
│   ├── drivers/    keyboard, screen, terminal
│   ├── shell/      shell, line_editor, parse, commands
│   ├── exec/       elf_loader, exec, images, programs
│   └── user/       hello.c, ticks.c, args.c, user_lib.h
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
```

Run with QEMU:

```bash
qemu-system-i386 -drive format=raw,file=build/img/os-image.bin
```

**Important:** use `-drive format=raw` (hard disk mode). Do not use `-fda` (floppy). LBA extended reads are not supported on floppy drives.

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

Current ramdisk programs: `hello`, `ticks`, `args`, `readline`

---

## Architecture Overview

```text
BIOS
 → boot.asm              load loader2 (CHS, 4 sectors)
 → loader2.asm           load kernel (LBA) + ramdisk (LBA) → protected mode
 → kernel_entry.asm      zero BSS → kernel_main()

kernel_main()
 → gdt_init()            install GDT: null, k-code, k-data, u-code, u-data, TSS
 → paging_init()         enable paging, identity-map 8 MB
 → memory_init()         bump allocator at 0x100000 (kernel structures)
 → pmm_init()            bitmap allocator at 0x200000 (user frames)
 → keyboard/timer/idt    drivers and interrupt table
 → sti
 → ramdisk_init()        validate ramdisk at 0x10000
 → shell_init()          interactive shell

runelf hello
 → ramdisk_find()        locate ELF in archive
 → process_pd_create()   fresh page directory, kernel entries shared
 → map ELF segments      pmm_alloc_frame() per page, PAGE_USER at 0x400000
 → map user stack        pmm_alloc_frame(), PAGE_USER at 0xBFFFF000
 → alloc kernel stack    pmm_alloc_frame(), tss_set_kernel_stack(frame + PAGE_SIZE)
 → setjmp()              save kernel context for sys_exit return
 → paging_switch(pd)     load process CR3
 → iret into ring 3      CS=0x1B, SS=0x23, EIP=e_entry
 → [program runs]
 → sys_exit() → int 0x80 → elf_process_exit()
 → paging_switch(kpd)    restore kernel CR3
 → process_pd_destroy()  free all PMM frames (ELF pages + stack)
 → pmm_free_frame()      free kernel stack frame
 → longjmp()             return to shell
```

---

## Physical Memory Layout

```text
0x00007C00   bootloader stage 1
0x00008000   loader2 stage 2 (done after jump to kernel)
0x00001000   kernel image
0x00006000   kernel .bss start (page tables here)
0x0000A008   kernel .bss end (approx)
0x00010000   ramdisk (permanent)
0x00090000   kernel stack top (grows downward)
0x00100000   bump allocator base — kernel structures
               kmalloc()      — argv arrays, parse buffers
               kmalloc_page() — process PDs, page tables
0x00200000   PMM base — user page frames
               pmm_alloc_frame() — ELF segment frames, user stack frames,
                                   kernel stack frames (per-process)
0x00800000   PMM ceiling (= identity-map limit)
0x00400000   USER_CODE_BASE — user ELF virtual address (per-process mapping)
0xBFFFF000   user stack virtual address (per-process mapping)
```

---

## Disk Image Layout

```text
LBA 0         boot.bin         (512 bytes)
LBA 1–4       loader2.bin      (2048 bytes)
LBA 5+        kernel.bin       (padded to 512-byte boundary)
after kernel  ramdisk.rd
```

---

## ELF Execution Model

* ELF programs compiled and linked at `0x400000` (`USER_CODE_BASE`) using `-Ttext-segment`
* Stored in `ramdisk.rd` — packed at build time by `mkramdisk`
* Each `runelf` creates a private page directory; the same virtual address maps to different physical frames per process
* User frames allocated from PMM (`0x200000`–`0x7FFFFF`) — reclaimed on exit
* argv strings copied into user stack memory before `iret` — ring-3 accessible
* Programs exit via `sys_exit()` which longjmps back to the shell

To add a program: add compile + link rules to Makefile, add `name:path` to the `ramdisk.rd` rule.

---

## GDT Layout

```text
Index 0   0x00   null
Index 1   0x08   kernel code   DPL=0
Index 2   0x10   kernel data   DPL=0
Index 3   0x1B   user code     DPL=3
Index 4   0x23   user data     DPL=3
Index 5   0x28   TSS           32-bit available
```

---

## Syscall ABI

```text
eax = syscall number
ebx = arg1
ecx = arg2
edx = arg3
return → eax

int 0x80 gate: DPL=3 (callable from ring 3)
```

---

## Current Limitations

* No scheduler — shell blocks during program execution
* ELF programs linked at fixed address 0x400000 — no PIE/relocation
* No filesystem — programs must be in ramdisk at build time
* Heap is bump-only (no free) — suitable for kernel structures, not user allocations
* `kmalloc_page`-allocated page tables (for non-ELF PDEs) are not freed per-process

---

## Direction

Next steps:

* Process abstraction — `process_t` struct with saved register state, kernel stack pointer, PD pointer
* Preemptive scheduler — timer IRQ context switch
* Filesystem-backed storage (FAT12 or custom FS via ATA PIO)

---

## License

Personal / educational project.