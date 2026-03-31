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

`terminal_put_uint` and `terminal_put_hex` are declared in `terminal.h` and defined in `terminal.c`. Do not add `static void terminal_put_uint(...)` in any other file — the compiler will reject it as a conflicting declaration.

---

## Bootloader Rules

* `boot.asm` must be **exactly 512 bytes**, ending with `dw 0xAA55`
* `loader2.asm` must be **exactly 2048 bytes**
* `kernel.bin` must be padded to a 512-byte sector boundary in the image (Makefile handles this)
* `RAMDISK_LBA` is computed as `5 + kernel_sectors` — if the padding step is skipped, the ramdisk will not load

---

## GDT Rules

Kernel must load its own GDT as the **first act** of `kernel_main()`. Do NOT rely on the loader2 GDT.

The GDT has 6 entries:

```text
0  null
1  kernel code  DPL=0  (0x08)
2  kernel data  DPL=0  (0x10)
3  user code    DPL=3  (0x1B)
4  user data    DPL=3  (0x23)
5  TSS          DPL=0  (0x28)
```

The task register must be loaded with `ltr 0x28` after writing the TSS descriptor. This is done in `gdt_init()` via `tss_flush()`.

If you add GDT entries, update the `gdt[N]` array size in `gdt.c` and the `gp.limit` computation — it is `sizeof(gdt) - 1` and is computed automatically.

---

## TSS Rules

`tss_set_kernel_stack(esp0)` must be called before `setjmp` and before every `iret` into ring 3. The value must be `kernel_stack_frame + PAGE_SIZE` — the top of a dedicated PMM frame allocated per process. Never point TSS ESP0 at the current C call frame — the CPU will overwrite live kernel stack data on the first syscall.

`TSS.SS0` must equal the kernel data selector (`0x10`). This is set once in `gdt_init()`.

The task register does **not** need to be reloaded when `tss.esp0` changes — writing to the struct in place is sufficient.

---

## Paging Rules

### BSS must be zeroed before paging_init() and pmm_init()

Page tables and the PMM bitmap live in `.bss`. Without zeroing they contain garbage and the CPU triple-faults (page tables) or the PMM incorrectly marks all frames as used (bitmap).

### paging_map_page() takes a page directory pointer

```c
void paging_map_page(u32* pd, u32 virt, u32 phys, u32 flags);
```

Pass `paging_get_kernel_pd()` to modify the kernel's own mappings. Pass a process PD to set up user mappings. Never pass NULL.

### Page table allocator split

`paging_map_page` uses different allocators depending on the PD index being filled:

* **PD index 1** (`USER_PD_INDEX`, ELF region): page table from `pmm_alloc_frame()` — so `process_pd_destroy()` can free it
* **All other indices**: page table from `kmalloc_page()` — kernel-owned, not freed per-process

Do not change this logic without updating `process_pd_destroy()` accordingly.

### Switching CR3

`paging_switch(pd)` flushes the entire TLB. After switching to a process PD, only memory mapped in that directory is accessible. After the process exits, `paging_switch(paging_get_kernel_pd())` must be called before touching any kernel data structures. This happens inside `elf_process_exit()`.

---

## Memory Allocator Rules

The system has two physical allocators with disjoint ranges:

```text
kmalloc / kmalloc_page   0x100000 – 0x1FFFFF   kernel structures (no free)
pmm_alloc_frame          0x200000 – 0x7FFFFF   user frames (freed on process exit)
```

**Never use `kmalloc_page` for user ELF frames, user stack frames, or kernel stack frames.** Those must come from `pmm_alloc_frame()` so they can be reclaimed on exit.

**Never use `pmm_alloc_frame` for kernel structures** (PDs, PTs, argv buffers). Those are intentionally permanent.

**PMM ceiling is the identity-map limit (0x800000 = 8 MB).** Frames above this address are not identity-mapped — the kernel cannot access them as pointers. `PMM_BASE + PMM_SIZE` must not exceed `0x800000`.

**Verify after changes with `meminfo`:**

```
meminfo              ← note free frame count
runelf hello
meminfo              ← count must be unchanged (frames fully reclaimed)
```

---

## ELF Loader Rules

* All user ELFs must be linked at `USER_CODE_BASE` (0x400000) using `-Ttext-segment`
* User frames come from `pmm_alloc_frame()` — not `kmalloc_page()`
* Segments are copied via physical frame addresses (identity-mapped) while still in the kernel address space, before CR3 is switched
* `tss_set_kernel_stack()` must be called **before** `setjmp()` — TSS ESP0 must be valid before the process could possibly trigger a syscall
* `setjmp()` must be called before `paging_switch()` and `iret` — it saves the kernel context that `longjmp()` will restore on `sys_exit`
* argv strings must be copied onto the user stack before `iret` — kernel heap memory is supervisor-only and inaccessible from ring 3

```bash
# Correct link command for user programs
i686-elf-ld -m elf_i386 -Ttext-segment 0x400000 -e _start myprog.o -o myprog.elf
```

Use `-Ttext-segment`, not `-Ttext`. `-Ttext` sets the address of `.text` but the linker
inserts a preceding ELF header segment at `0x3FF000` (PD index 0), which shares the kernel
page table and is never reclaimed by `process_pd_destroy()`.

---

## Ring 3 Rules

* The `int 0x80` IDT gate must have DPL=3 (`IDT_FLAG_INT_GATE_USER`). DPL=0 causes a #GP when ring-3 code tries to invoke it.
* User data selector (0x23) must be loaded into DS/ES/FS/GS before `iret` so the first user memory access uses the correct descriptor.
* User programs must call `sys_exit()` before returning from `_start`. A bare `ret` from `_start` has no valid return address and will fault.
* `sys_exit()` → `elf_process_exit()` → `longjmp()` is the only valid exit path. There is no kernel frame to return through after `iret`.

---

## Interrupt Rules

### After `sti`, everything must be correct

Checklist:
* GDT loaded (kernel-owned, `gdt_init()` called first)
* IDT loaded
* PIC remapped
* IRQ handlers installed and valid
* TSS loaded (`ltr` executed)

Failure → `#GP → #DF → triple fault → reboot`.

---

## Syscall Rules

If you touch `isr128_stub`, you MUST update `syscall_regs_t` to match.

The struct field order is determined by the assembly push order. See `syscalls.md` for the full explanation.

---

## Debugging Strategy

### Early boot (before terminal)

```asm
mov byte [0xB8000], 'X'
mov byte [0xB8001], 0x4F   ; red background
```

### Kernel stage

```c
terminal_puts("debug\n");
terminal_put_uint(some_value);
terminal_putc('\n');
```

### Inside a user process (ring 3)

```c
sys_write("debug\n", 6);
sys_putc('X');
```

Do not call `terminal_puts` from user programs — it is a kernel function in supervisor memory, inaccessible from ring 3.

### Memory accounting

```
meminfo
```

Run before and after `runelf` to confirm frames are reclaimed. If the free count drops and doesn't recover, there is a leak in `process_pd_destroy()`, a missed `pmm_free_frame()` call, or a user ELF linked with `-Ttext` instead of `-Ttext-segment`.

### QEMU logging

```bash
qemu-system-i386 -drive format=raw,file=build/img/os-image.bin \
    -no-reboot -no-shutdown \
    -d int,cpu_reset,guest_errors \
    -D qemu.log
```

Useful signals in the log:

* `v=0e` at `cpl=3` — page fault from ring 3 (check CR2 for faulting address)
* `v=0d` — general protection fault (bad selector, privilege violation)
* `v=08` — double fault (kernel stack corrupt, bad GDT/IDT)
* `TR=0000` — task register not loaded (`ltr` not called)
* `CPU Reset` → triple fault
* Repeated `INT 0x20` at same EIP — timer fires but execution is stuck

---

## Common Failure Modes

### 1. Reboot loop

Bad GDT, bad IDT, or broken interrupt handler. Confirm `gdt_init()` is the first call in `kernel_main()`.

### 2. Triple fault on boot

BSS not zeroed before `paging_init()`.

### 3. "ramdisk: bad magic"

`kernel.bin` not padded to 512-byte boundary, or launched with `-fda`.

### 4. Red '8' on screen

Double fault. Common causes: corrupt kernel stack (wrong TSS ESP0), bad IDT entry, privilege violation during interrupt handling.

### 5. Crash after iret into ring 3

* TSS not loaded (`ltr` not called in `gdt_init`)
* TSS ESP0 not set from a dedicated PMM frame — must be `kernel_stack_frame + PAGE_SIZE`
* User code/data GDT entries missing or wrong DPL
* `int 0x80` IDT gate has DPL=0 instead of DPL=3

### 6. argv reads garbage in ring 3

argv strings are on the kernel heap — supervisor-only, inaccessible from ring 3. Must be copied onto the user stack before `iret`.

### 7. Program runs but shell doesn't return

`sys_exit()` not called, or `longjmp` context corrupted. Verify `setjmp` is called before `paging_switch` and that TSS ESP0 does not point into the setjmp save area.

### 8. Syscalls silently broken

`syscall_regs_t` struct mismatch with `isr128_stub` push order. See `syscalls.md`.

### 9. runelf crashes / reboots

If `runelf` worked before the PMM was added and now crashes: verify that `PMM_BASE` (0x200000) > the bump allocator's high-water mark at the time `pmm_alloc_frame()` is first called. If they overlap, both allocators return the same frame address → corruption. Use `meminfo` to inspect the heap top.

### 10. "pmm: double free" warning

`process_pd_destroy()` called twice on the same PD, or a frame was mapped into multiple PTEs. Check that `s_current_pd` is cleared to 0 after every call to `process_pd_destroy`.

### 11. PMM frame count leaks after runelf

User ELF was linked with `-Ttext` instead of `-Ttext-segment`. This places a PT_LOAD segment
at `0x3FF000` (PD index 0), which shares the kernel page table and is never reclaimed by
`process_pd_destroy()`. Always use `-Ttext-segment 0x400000`.

---

## Safe Development Order

When adding features:

1. Build — fix compile errors first
2. Boot — verify shell appears
3. Verify interrupts — run `uptime`, confirm ticks advance
4. Test `runelf hello` — confirm ring-3 execution, argc/argv, and clean shell return
5. Run `meminfo` before and after `runelf` — confirm free frame count is unchanged
6. Then expand

---

## Adding a New User Program

1. Create `src/user/myprog.c` with `void _start(int argc, char** argv)`
2. End `_start` with `sys_exit(0)` — do not rely on returning
3. Add compile rule to Makefile
4. Add link rule at `-Ttext-segment 0x400000`  ← not `-Ttext`
5. Add to ramdisk rule
6. `make clean && make`
7. `runelf myprog`

All programs share `-Ttext-segment 0x400000` safely — each gets its own page directory.

---

## Coding Style

* Keep things simple
* Prefer explicit over clever
* Avoid hidden magic
* Debug first, optimize later

---

## Next Steps (Recommended)

* `SYS_READ` — keyboard input buffer in `keyboard.c`, blocking read syscall for user programs
* Process abstraction (`process_t`) — saved register state, kernel stack pointer, PD pointer, name field
* Preemptive scheduler — timer IRQ context switch using per-process kernel stacks
* Filesystem — FAT12 or custom FS via ATA PIO

---

## Final Rule

If something breaks:

👉 Assume **you violated a contract** (ABI, memory layout, paging, privilege level, allocator invariants, or hardware expectations).

Then trace from there.