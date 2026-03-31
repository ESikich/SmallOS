# Development Guide

This document explains how to safely work on the OS without breaking it.

---

## Build Workflow

```bash
make clean && make
qemu-system-i386 -drive format=raw,file=build/img/os-image.bin
```

Do not use `-fda` (floppy). LBA extended reads require hard disk mode.

---

## Project Rules

### 1. Headers must NOT define functions

### 2. One definition per symbol

### 3. Never include `.c` files

### 4. Always include required headers

If you see `implicit declaration of function` — you forgot a header.

### 5. No private copies of shared utilities

`terminal_put_uint` and `terminal_put_hex` are declared in `terminal.h` and defined in `terminal.c`. Do not add private copies in any other file.

---

## Bootloader Rules

* `boot.asm` must be **exactly 512 bytes**, ending with `dw 0xAA55`
* `loader2.asm` must be **exactly 2048 bytes**
* `kernel.bin` must be padded to a 512-byte sector boundary in the image (Makefile handles this)
* `RAMDISK_LBA` is computed as `5 + kernel_sectors` — if padding is skipped, the ramdisk will not load

---

## GDT Rules

Kernel must load its own GDT as the **first act** of `kernel_main()`. The GDT has 6 entries (null, k-code, k-data, u-code, u-data, TSS). The task register must be loaded with `ltr 0x28` in `gdt_init()`. If you add entries, update the array size and `gp.limit`.

---

## TSS Rules

`tss_set_kernel_stack(esp0)` must be called before `setjmp` and before every `iret` into ring 3. The value must be `kernel_stack_frame + PAGE_SIZE`. On process exit, `elf_process_exit` restores it to `0x90000` (the static kernel stack top for the shell).

`tss_get_esp0_ptr()` returns `&tss.esp0` and is used by the scheduler to update TSS directly during context switches. Do not remove this function.

---

## Paging Rules

### BSS must be zeroed before paging_init() and pmm_init()

### paging_map_page() takes a page directory pointer

```c
void paging_map_page(u32* pd, u32 virt, u32 phys, u32 flags);
```

### Process page tables must be PMM-backed

* Process page directories come from `pmm_alloc_frame()`
* Any process-private user page table comes from `pmm_alloc_frame()`
* Kernel-shared page tables remain shared from `kernel_page_directory`

Do not allocate process-owned paging structures with `kmalloc_page()`. The bump allocator has no free path and will leak across `runelf` invocations.

### Switching CR3

`paging_switch(pd)` flushes the TLB. After switching to a process PD, only memory mapped in that directory is accessible. Always switch back to the kernel PD before touching kernel data structures.

---

## Memory Allocator Rules

```text
kmalloc / kmalloc_page   0x100000 – 0x1FFFFF   permanent (no free)
pmm_alloc_frame          0x200000 – 0x7FFFFF   reclaimable on process exit
```

`kmalloc` / `kmalloc_page` are for permanent kernel structures only. Do not use them for short-lived shell parsing state or other per-command allocations.

`pmm_alloc_frame` is for everything reclaimed on exit: `process_t` structs, process page directories, ELF frames, stack frames, all process-private page tables, and kernel stack frames.

**Verify after changes with `meminfo`:**

```
meminfo              ← note free frame count
runelf hello
meminfo              ← count must be unchanged
```

### Shell parser rule

`parse_command()` must not allocate from the bump heap. It tokenizes the mutable shell input buffer in place, stores pointers in a fixed-size `argv[MAX_ARGS]` array, and must not call `kmalloc()`.

Why:

* Per-command `kmalloc()` use leaks permanently because the bump allocator has no free path
* Repeated shell commands would otherwise increase `heap used` even when PMM accounting is correct
* Zero-allocation parsing keeps shell memory usage stable and makes `meminfo` output trustworthy during repeated `runelf` tests

### Shell parser rule

...existing text...

### Process Data Ownership Rules

* Do not rely on shell input buffers for process data.
* All user-process arguments must be copied into process-owned storage before execution.
* Pointers passed into a process must remain valid for the entire lifetime of that process.

Rationale:
Shell input buffers are mutable and reused. Storing pointers into them leads to
use-after-modification bugs once the shell continues execution.

---

## Scheduler Rules

`sched_init()` must be called **after `idt_init()` and before `sti`**. It initialises the scheduler table and wires `tss_esp0_ptr`. The shell is not registered here — it is created later in `kernel_main()` and added with `sched_enqueue()`. If called after `sti`, the first timer tick may fire with an uninitialised scheduler state.

`sched_enqueue(proc)` — call after `proc->state = RUNNING`, before `iret`. If the table is full the process still runs but is not preempted.

`sched_dequeue(proc)` — call before `paging_switch` and `process_destroy` in `elf_process_exit`. It removes the process from the run queue, compacts the table, and adjusts scheduler indices so round-robin execution can continue over the remaining runnable entries.

**EOI ordering in `irq0_handler_main`**: EOI is sent **before** `sched_tick`. If `sched_switch` lands on a different context and `irq0_handler_main` never returns, EOI must already be sent. Do not move it after `sched_tick`.

**`irq0_stub` passes ESP to C**: `irq0_stub` pushes ESP and passes it to `irq0_handler_main`. This ESP is the address of the register frame on the kernel stack — the scheduler saves it as the resume point. If you modify `irq0_stub`, ensure ESP is still passed correctly.

**`sched_switch` argument loading**: `sched_switch.asm` loads all four arguments (`save_esp`, `next_esp`, `next_cr3`, `next_esp0`) into registers **before** modifying ESP. Any change to the argument order must update both the C caller and the assembly. The arguments must all be read from the old stack before `esp` is replaced.

**`sched_esp == 0` guard**: the scheduler skips slots with `sched_esp == 0` — this prevents switching to a process that has never been preempted and has no valid resume stack. Do not remove this guard.

---

## Process Rules

`process_create(name)` allocates from PMM. Fill `pd`, `kernel_stack_frame`, and `sched_esp` (leave 0 — set by first preemption) before launching.

`process_destroy(proc)` frees PD, kernel stack frame, and the process_t frame. After this call `proc` is dangling.

`elf_process_exit()` must save `&proc->exit_ctx` before calling `process_destroy()`.

---

## ELF Loader Rules

* Link user ELFs with `-Ttext-segment 0x400000`, not `-Ttext`
* User frames from `pmm_alloc_frame()` only
* `tss_set_kernel_stack()` before `setjmp()` before `paging_switch()` before `iret`
* `sched_enqueue()` after `proc->state = RUNNING`, before `iret`
* `sched_dequeue()` as the first action in `elf_process_exit()`

---

## Interrupt Rules

### After `sti`, everything must be valid

* GDT loaded
* IDT loaded
* PIC remapped
* All IRQ handlers installed
* TSS loaded (`ltr` executed)
* Scheduler initialised (`sched_init()` called)

Failure → `#GP → #DF → triple fault → reboot`.

### IRQ EOI must be sent before any handler that may not return

Both `irq0_handler_main` and `irq1_handler_main` send EOI as their first meaningful action. Do not move EOI below `sched_tick` or `keyboard_handle_irq`.

---

## Syscall Rules

If you touch `isr128_stub`, you MUST update `syscall_regs_t` to match. The struct field order is determined by the assembly push order. See `syscalls.md`.

---

## Debugging Strategy

### Early boot

```asm
mov byte [0xB8000], 'X'
mov byte [0xB8001], 0x4F
```

### Kernel stage

```c
terminal_puts("debug\n");
terminal_put_uint(value);
```

### Inside ring 3

```c
sys_write("debug\n", 6);
```

### Memory accounting

```
meminfo   ← before and after runelf
```

### QEMU logging

```bash
qemu-system-i386 -drive format=raw,file=build/img/os-image.bin \
    -no-reboot -no-shutdown \
    -d int,cpu_reset,guest_errors \
    -D qemu.log
```

Useful signals:
* `v=0e` at `cpl=3` — page fault from ring 3
* `v=0d` — general protection fault
* `v=08` — double fault
* `TR=0000` — task register not loaded
* `CPU Reset` — triple fault
* Repeated `INT 0x20` at same EIP — timer stuck

---

## Common Failure Modes

| # | Symptom | Cause |
|---|---|---|
| 1 | Reboot loop | Bad GDT or IDT |
| 2 | Triple fault on boot | BSS not zeroed |
| 3 | "ramdisk: bad magic" | kernel.bin not sector-padded, or `-fda` mode |
| 4 | Red '8' on screen | Double fault — bad TSS ESP0 or corrupt stack |
| 5 | Crash after iret | TSS not loaded; wrong TSS ESP0; bad user GDT entries; DPL=0 on int 0x80 gate |
| 6 | argv garbage in ring 3 | Strings not copied to user stack before iret |
| 7 | Shell doesn't return | sys_exit not called, or longjmp context corrupted |
| 8 | Syscalls silently broken | syscall_regs_t mismatch |
| 9 | PMM leak after runelf | ELF linked with `-Ttext` not `-Ttext-segment` |
| 10 | "pmm: double free" | process_destroy called twice; call process_set_current(0) after destroy |
| 11 | Crash on first context switch | sched_esp==0 guard missing; or sched_init not called before sti |
| 12 | Timer fires but no preemption | EOI sent after sched_tick; or irq0_stub not passing ESP |
| 13 | System freezes after context switch | EOI not sent before sched_switch; IRQ0 permanently masked |

---

## Safe Development Order

1. Build — fix compile errors
2. Boot — confirm shell appears
3. Verify timer — run `uptime`, confirm ticks advance
4. Test `runelf hello` — confirm exit is clean
5. Run `meminfo` before and after — confirm frame count unchanged
6. Then expand

---

## Adding a New User Program

1. Create `src/user/myprog.c` with `void _start(int argc, char** argv)`
2. End with `sys_exit(0)`
3. Add compile + link rules to Makefile (`-Ttext-segment 0x400000`)
4. Add to ramdisk rule
5. `make clean && make`
6. `runelf myprog`

---

## Coding Style

* Keep things simple
* Prefer explicit over clever
* Avoid hidden magic
* Debug first, optimize later

---

## Next Steps (Recommended)

* Filesystem — FAT12 or custom FS via ATA PIO
* `SYS_YIELD` / `SYS_EXEC` syscalls

---

## Final Rule

If something breaks:

👉 Assume **you violated a contract** (ABI, memory layout, paging, privilege level, allocator invariants, scheduler invariants, or hardware expectations).

Then trace from there.