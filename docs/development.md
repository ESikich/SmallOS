# Development Guide

This document explains how to safely work on the OS without breaking it.

---

## Build Workflow

```bash
make clean && make
qemu-system-i386 -drive format=raw,file=build/img/os-image.bin
```

On Windows / PowerShell, this pattern keeps a GTK window open and captures both console output and QEMU logs:

```powershell
qemu-system-i386 -drive format=raw,file=os-image.bin -m 32 -serial stdio -d int,cpu_reset,guest_errors -D qemu.log -display gtk 2>&1 | Tee-Object -FilePath qemu-console.log
```

`make test` runs the same image headlessly, launches the shell `selftest`
command, feeds the interactive `readline` prompt, and verifies both the
built-in shell commands (`tests/shell/`) and shipped ELFs
(`tests/elfs/`).

The scripted shell/selftest tables are kept in static storage because the
shell task only has a 4 KB kernel stack. If you add more scripted command
cases, keep the large tables off the stack so the process state stays safe
during the full regression run.

`make smoke` runs the dedicated reboot/halt checks.  Use
`make smoke-reboot` or `make smoke-halt` when you want to verify those
commands without running the full guest suite.

`kernel_main()` now runs a small startup selfcheck right after `pmm_init()`.
It prints `kernel: selfcheck PASS` when the live TSS selector, boot stack,
heap base, and PMM free-frame baseline all match the boot contract. If any of
those invariants drift, the kernel halts before the shell starts.

Do not use `-fda` (floppy). LBA extended reads require hard disk mode.

`help` is table-driven:
- built-in shell commands are listed from the shell command table
- shipped ELF programs are listed from the program table
- both sections carry short descriptions, so keep the tables and the help output in sync when adding or removing entries

---

## Project Rules

### 1. Headers must NOT define functions

### 2. One definition per symbol

### 3. Never include `.c` files

### 4. Always include required headers

If you see `implicit declaration of function` — you forgot a header.

### 5. No private copies of shared utilities

`terminal_put_uint` and `terminal_put_hex` are declared in `terminal.h` and defined in `terminal.c`. Do not add private copies in any other file.

`src/kernel/klib.h` / `src/kernel/klib.c` are now the canonical home for shared freestanding string and memory helpers such as `k_memcpy`, `k_memset`, `k_strlen`, `k_strcmp`, `k_strncpy`, and `k_starts_with`. If a helper is generally useful across more than one file, it belongs in `klib` rather than as a file-local static copy.

---

## klib

`klib` is the shared freestanding kernel utility library in `src/kernel/klib.h` and `src/kernel/klib.c`.

Use it for basic string and memory primitives that would normally come from libc, but must exist in the kernel without a hosted C runtime. The current exported helpers are:

* `k_memcpy(dst, src, n)` — byte copy
* `k_memset(dst, val, n)` — byte fill
* `k_strlen(s)` — string length
* `k_strcmp(a, b)` — equality check (`1` if equal, `0` otherwise)
* `k_strncpy(dst, src, n)` — bounded copy that always null-terminates when `n > 0`
* `k_starts_with(s, prefix)` — prefix check

### klib rules

* Use the `k_` prefix for shared freestanding helpers so call sites are clearly kernel-local and do not collide with any future libc-facing wrappers.
* New shared string / memory primitives should be added to `klib`, not reintroduced as private statics in individual `.c` files.
* Keep file-local helpers file-local only when they are truly specific to one implementation and are not general-purpose utilities.

### 6. The keyboard driver must not know about processes, the scheduler, or the shell

`keyboard.c` decodes scancodes and calls the registered `keyboard_consumer_fn`. That is its entire job.

Routing decisions — whether input goes to the shell or a user process — belong to the consumer, not the driver. The consumer is registered via `keyboard_set_consumer()`:
- `shell_init()` registers `shell_key_consumer`
- `process_set_foreground(proc)` registers `process_key_consumer`
- `process_set_foreground(0)` restores the shell consumer via `shell_register_consumer()`

Do not add imports of `process.h`, `scheduler.h`, or `shell.h` to `keyboard.c` or `keyboard.h`. Do not add routing logic to `keyboard_handle_irq()`. If a new input consumer is needed, register it — do not modify the driver.

---

### 7. Build scripts must discover layout facts from the files that own them

Examples:

* `boot.asm` owns `BOOT_SECTOR_SIZE`, `MBR_PARTITION_TABLE_OFFSET`, and `MBR_PARTITION_ENTRY_SIZE`
* `loader2.asm` owns `LOADER2_SIZE_BYTES`
* `mkfat16.c` owns FAT image size constants

The Makefile should read those declarations and pass them into host tools such as `mkimage`. Do not reintroduce anonymous layout numbers into the Makefile.

---
* `boot.asm` must be **exactly 512 bytes**, ending with `dw 0xAA55`
* `loader2.asm` must be **exactly 2048 bytes**
* `kernel.bin` must be padded to a 512-byte sector boundary during final image assembly (`mkimage` handles this)
* `kernel_lba` is derived as `1 + loader2_sectors`
* `FAT16_LBA` is computed as `kernel_lba + kernel_sectors` and written into the FAT16 partition entry — if padding is skipped, FAT16 reads will return incorrect data

### Loader2 address invariant

Loader2 is currently at `0x40000`. The kernel loads to `0x1000`. The stage-2 stack top is generated by the build (`SP=0xFF00`, physical top `0x4FF00`). The kernel must not grow large enough to overwrite loader2 or the stack during the INT 0x13 read:

```text
safe kernel size = (0x40000 - 0x1000) / 512 sectors
                 = 504 sectors = 252 KiB
```

The required invariant is that `0x1000 + kernel_sectors * BOOT_SECTOR_SIZE` must stay below both the loader2 load address and the generated stage-2 stack top. If the kernel exceeds 504 sectors, the build fails before image assembly. To raise the ceiling further, move stage 2 higher and update the matching constants in `Makefile` and `loader2.asm`.

**Symptom of violation**: BIOS INT 0x13 hangs silently mid-transfer at `Loading...` with no error message printed.

`make boot-layout-check` is the host-side guard for this contract. It verifies the built loader and kernel artifacts, the generated stage-2 stack values, and the current ceiling before the disk image is assembled.

`make image-layout-check` then validates the finished `os-image.bin`, including the patched FAT16 LBA and the sector placement of each component.

---

## GDT Rules

Kernel must load its own GDT early in `kernel_main()`, before interrupts are enabled. The GDT has 6 entries (null, k-code, k-data, u-code, u-data, TSS). The task register must be loaded with `ltr 0x28` in `gdt_init()`. If you add entries, update the array size and `gp.limit`.

---

## TSS Rules

`tss_set_kernel_stack(esp0)` is the supported interface for updating TSS.ESP0.

It must be called before every `iret` into ring 3. For a process-owned kernel stack, the value must be `kernel_stack_frame + PAGE_SIZE`. `elf_user_task_bootstrap()` sets it before first ring-3 entry for a user task, and scheduler-driven context switches apply the incoming process's `next_esp0` through `sched_switch` via `tss_set_kernel_stack()`.

Do not reintroduce `tss_get_esp0_ptr()` or any pointer-based access into the packed TSS structure.

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

`paging_switch(pd)` flushes the TLB. After switching to a process PD, only memory mapped in that directory is accessible. In the current design, process page directories copy the kernel mappings (all kernel PDEs except the private ELF region at PD index 1), so kernel code/data, VGA, heap, and stack remain accessible after the switch. Switch back to the kernel PD only when you specifically need the master kernel address space rather than the process-owned one.

---

## Memory Allocator Rules

```text
kmalloc / kmalloc_page   0x100000 – 0x1FFFFF   permanent (no free)
pmm_alloc_frame          0x200000 – 0x7FFFFF   reclaimable on process exit
```

`kmalloc` / `kmalloc_page` are for permanent kernel structures only. Do not use them for transient buffers — the bump allocator has no free path and heap used will grow permanently with each call.

`pmm_alloc_frame` is for everything reclaimed on exit: `process_t` structs, process page directories, ELF frames, stack frames, all process-private page tables, and kernel stack frames.

### FAT16 load buffer rule

`fat16_load()` uses a static BSS buffer (`s_load_buf[256 KB]`), not `kmalloc`. Do not change this to `kmalloc` — each `runelf` call would permanently consume heap.

**Verify after changes with `meminfo`:**

```text
meminfo              ← note heap top and free frame count
runelf apps/demo/hello
meminfo              ← heap top and frame count must be identical
runelf apps/demo/hello
meminfo              ← still identical after second run
```

### Shell parser rule

`parse_command()` must not allocate from the bump heap. It tokenizes the mutable shell input buffer in place, stores pointers in a fixed-size `argv[MAX_ARGS]` array, and must not call `kmalloc()`.

### Process Data Ownership Rules

* Do not rely on shell input buffers for process data.
* All user-process arguments must be copied into process-owned storage before execution.
* Pointers passed into a process must remain valid for the entire lifetime of that process.

---

## Scheduler Rules

`sched_init()` must be called **after `idt_init()` and before `sti`**. It initialises the scheduler table. The shell is not registered here — it is created later in `kernel_main()` and added with `sched_enqueue()`. If called after `sti`, the first timer tick may fire with an uninitialised scheduler state.

`sched_enqueue(proc)` — call after `proc->state = RUNNING` when handing a task to the scheduler. The shell task follows this path in `kernel_main()`, and ELF launches now do as well.

`sched_dequeue(proc)` is for scheduler-owned tasks. In the current tree it is used from `sched_exit_current()`. It removes the process from the run queue, compacts the table, and adjusts scheduler indices so round-robin execution can continue over the remaining runnable entries.

**EOI ordering in `irq0_handler_main`**: EOI is sent **before** `sched_tick`. If `sched_switch` lands on a different context and `irq0_handler_main` never returns, EOI must already be sent. Do not move it after `sched_tick`.

**`irq0_stub` passes ESP to C**: `irq0_stub` pushes ESP and passes it to `irq0_handler_main`. This ESP is the address of the register frame on the kernel stack — the scheduler saves it as the resume point. If you modify `irq0_stub`, ensure ESP is still passed correctly.

**`sched_switch` argument loading**: `sched_switch.asm` loads all four arguments (`save_esp`, `next_esp`, `next_cr3`, `next_esp0`) into registers **before** modifying ESP. Any change to the argument order must update both the C caller and the assembly. The arguments must all be read from the old stack before `esp` is replaced.

**`sched_esp == 0` guard**: the scheduler skips slots with `sched_esp == 0` — this prevents switching to a process that has never been preempted and has no valid resume stack. Do not remove this guard.

---

## Process Rules

`process_create(name)` allocates from PMM. Fill `pd`, `kernel_stack_frame`, and `sched_esp` (leave 0 — set by first preemption) before launching.

`process_destroy(proc)` frees PD, kernel stack frame, and the process_t frame. After this call `proc` is dangling.

Exited tasks must be marked `PROCESS_STATE_ZOMBIE` and destroyed later from a safe stack. Do not free a task from inside `sched_exit_current()`, because the kernel is still running on that task's kernel stack.

---

## ELF Loader Rules

* Link user ELFs with `-Ttext-segment 0x400000`, not `-Ttext`
* User frames from `pmm_alloc_frame()` only
* `elf_user_task_bootstrap()` sets `tss_set_kernel_stack()` before first ring-3 entry for a user task, and scheduler-driven switches update ESP0 for later entries
* Use `esp - 8` from the IRQ0 / syscall-stub paths when handing a resume frame to the scheduler
* Preserve the true interrupt/syscall resume ESP; do not let `sched_switch()` overwrite it with the scheduler's own C call-frame ESP

---

## ATA / FAT16 Rules

* `ata_init()` must be called before `fat16_init()` and before any `ata_read_sectors()` call
* `fat16_init()` must be called before `fat16_load()` or `fat16_ls()`
* `fat16_load()` returns a pointer into the static `s_load_buf` buffer — the caller must not hold this pointer across another `fat16_load()` call
* `elf_run_image()` copies all ELF segment data into PMM frames before returning, so the buffer is safe to reuse immediately after `elf_run_named()` returns

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

### IRQ EOI should be sent before IRQ-side work that must not leave the PIC masked

Both `irq0_handler_main` and `irq1_handler_main` send EOI as their first meaningful action. Do not move EOI below `sched_tick`; for IRQ1, keep it before `keyboard_handle_irq()` as the current handler ordering rule.

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

```text
meminfo   ← before and after runelf (heap and frames must be stable)
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
| 3 | "fat16: bad boot signature" | kernel.bin not sector-padded, FAT16 image missing, or `-fda` mode |
| 4 | Red '8' on screen | Double fault — bad TSS ESP0 or corrupt stack |
| 5 | Crash after iret | TSS not loaded; wrong TSS ESP0; bad user GDT entries; DPL=0 on int 0x80 gate |
| 6 | argv garbage in ring 3 | Strings not copied to user stack before iret |
| 7 | Shell doesn't return | child never reached ZOMBIE, or scheduler resume ESP bookkeeping is wrong |
| 8 | Syscalls silently broken | syscall_regs_t mismatch |
| 9 | PMM leak after runelf | ELF linked with `-Ttext` not `-Ttext-segment` |
| 10 | "pmm: double free" | process_destroy called twice |
| 11 | Crash on first context switch | sched_esp==0 guard missing; or sched_init not called before sti |
| 12 | Timer fires but no preemption | EOI sent after sched_tick; or irq0_stub not passing ESP |
| 13 | System freezes after context switch | EOI not sent before sched_switch; IRQ0 permanently masked |
| 14 | Hangs at "Loading." | Kernel too large — overwrites loader2 during INT 0x13 read; move loader2 higher |
| 15 | "fat16: bad MBR signature" / "fat16: partition entry not populated" | final image assembly failed; check the `mkimage` step and its arguments |
| 16 | Heap grows across runelf | fat16_load is using kmalloc instead of static buffer |
| 17 | "fat16: not found" | Filename not matching 8.3 uppercase format; check mkfat16 output |

---

## Safe Development Order

1. `make clean && make` — fix compile errors
2. Boot — confirm shell appears and `fat16: ok` prints
3. `ataread 0` — confirm `sig: 0x55 0xAA` and the correct FAT16 partition LBA value
4. `fsls` — confirm FAT16 root directory lists correctly
5. `mkdir TESTDIR` / `rmdir TESTDIR` — confirm directory creation and removal
6. `fsread apps/demo/hello.elf` — confirm `7F 45 4C 46` (ELF magic)
7. `runelf apps/demo/hello` — confirm ELF loads from FAT16 and exits cleanly
8. `meminfo` before and after — heap top and frame count must be identical
9. Run a second `runelf apps/demo/hello` — confirm static buffer reuse is safe
10. Then expand

---

## Adding a New User Program

1. Create `src/user/myprog.c` with `void _start(int argc, char** argv)`
2. End with `sys_exit(0)`
3. Add `myprog` to `USER_PROGS` in Makefile — automatically included in the FAT16 image
4. `make clean && make`
5. `runelf myprog`

---

## Coding Style

* Keep things simple
* Prefer explicit over clever
* Avoid hidden magic
* Debug first, optimize later

---

## Current Next Steps

* Broaden the syscall pointer regression coverage with page-boundary cases
* Consider smaller quality-of-life shell commands for debugging the new runtime contracts

---

## Final Rule

If something breaks:

👉 Assume **you violated a contract** (ABI, memory layout, paging, privilege level, allocator invariants, scheduler invariants, or hardware expectations).

Then trace from there.
