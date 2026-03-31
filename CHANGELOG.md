# Changelog

## [Current]

### Added

* `elf_loader.c` / `process.h`
  * Added scheduler-ready bootstrap state for ELF processes:
    * `user_entry`, `user_argc`, `user_argv`, and in-process argv storage
    * `elf_user_task_bootstrap()` for future scheduler-driven first entry
  * Added `elf_seed_sched_context()` to prepare a process for scheduler-owned execution
  * Arguments are now copied into `process_t` instead of relying on shell buffer pointers

### Notes

This is a preparatory step toward scheduler-owned ELF tasks.

* ELF processes still execute through the foreground `setjmp`/`longjmp` path
* The new scheduler bootstrap state is not yet used for execution

---

## [Previous] — shell promoted to a real kernel task (transitional execution model)

### Added

* `shell.c` / `shell.h`
  * Added a small shell event queue and `shell_poll()`
  * `shell_input_char()`, cursor movement, delete/backspace, and history navigation now enqueue work instead of mutating shell/editor state directly from IRQ1
  * Added `shell_task_main()` so the shell can run as an explicit schedulable kernel task

* `process.c` / `process.h`
  * Added `process_create_kernel_task(name, entry)`
  * Added `kernel_entry` to `process_t` and kernel-task bootstrap logic that seeds an initial `sched_esp` for first entry via `sched_switch()`

* `scheduler.c` / `scheduler.h`
  * Added `sched_start(first_proc)` to switch from the boot stack into the first runnable kernel task

### Changed

* `kernel.c`
  * Boot now creates the shell as an explicit kernel task and enters it with `sched_start()` instead of calling `shell_init()` directly on the boot stack

* `keyboard.c` / shell path
  * IRQ1 now feeds the shell through queued events; shell editing logic runs in task context instead of interrupt context

* `elf_loader.c` / `process.c`
  * Kept `runelf` on the older foreground `setjmp`/`longjmp` path for this increment
  * `process_get_current()` now prefers the explicit foreground process pointer while one is set, so `SYS_EXIT` tears down the user ELF process instead of the shell task
  * `elf_process_exit()` now executes `sti` before `longjmp()` so the shell resumes with interrupts enabled and keyboard IRQs continue to fire

### Notes

This is an intentional hybrid stage:

* the **shell** is scheduler-owned
* **ELF user processes** are still foreground-only

The next architectural step is to make `runelf` create real scheduler-owned user tasks and remove the remaining `setjmp`/`longjmp` foreground return path.

---

## [Previous] — zero-allocation shell command parsing

### Changed

* `parse.h` / `parse.c`
  * `command_t` now stores arguments in a fixed-size `argv[MAX_ARGS]` array instead of a heap-allocated `char**`
  * `parse_command()` now tokenizes the mutable input buffer in place instead of allocating argv storage and token copies

### Fixed

* **Per-command shell heap leak** — repeated commands such as `runelf hello` no longer grow `heap used` through parser-side `kmalloc()` allocations

### Documentation

* Updated `development.md` to document the zero-allocation parsing rule and clarify that bump-allocator memory is for permanent kernel structures only

---

## [Previous] — process-private page tables moved to PMM

### Changed

* `paging.c` / `paging.h`
  * `paging_map_page()` now allocates process-private page tables from `pmm_alloc_frame()` instead of `kmalloc_page()`
  * The kernel master page directory still keeps its long-lived kernel-owned mappings
  * `process_pd_destroy()` now frees every process-private page table it encounters in the user PDE range, not just the ELF-region table

### Fixed

* **Per-process paging leak** — process-private page tables, including the user stack page table, are now PMM-backed and reclaimed on process exit instead of accumulating in the kernel bump allocator

### Documentation

* Updated `README.md`, `architecture.md`, `memory.md`, `development.md`, and `execution.md` to describe the new paging ownership model and teardown behavior

---

## [Previous] — Preemptive round-robin scheduler

### Added

* `src/kernel/scheduler.h` / `src/kernel/scheduler.c` — round-robin scheduler
  * Fixed-size process table (`SCHED_MAX_PROCS = 8`); slot 0 is always the shell/idle context
  * `sched_init()` — registers the shell as slot 0, wires `tss_esp0_ptr` for the assembly switch helper; must be called before `sti`
  * `sched_enqueue(proc)` — adds a process to the run queue; called by `elf_run_image` just before `iret`
  * `sched_dequeue(proc)` — removes a process and resets to slot 0; called by `elf_process_exit` before `longjmp`
  * `sched_tick(esp)` — called from `irq0_handler_main` on every timer tick; after `SCHED_TICKS_PER_QUANTUM` ticks (10 = 100 ms at 100 Hz) picks the next runnable slot and calls `sched_switch`
  * `sched_current()` — returns the active `process_t*`
  * Guard: slots with `sched_esp == 0` are skipped — a brand-new process is not scheduled until its kernel stack has a valid resume point from its first preemption

* `src/kernel/sched_switch.asm` — low-level context switch
  * `sched_switch(save_esp, next_esp, next_cr3, next_esp0)` — loads all four arguments into registers before modifying ESP, then: writes current ESP to `*save_esp`; updates `tss.esp0` via `tss_esp0_ptr`; loads `next_cr3` into CR3 (flushing TLB); sets ESP to `next_esp`; `ret` (which pops the incoming context's return address and resumes it)
  * Resume path: `sched_switch ret` → `sched_tick` → `irq0_handler_main` → `irq0_stub iretd` → ring 3

* `src/kernel/process.h` — added `sched_esp` field to `process_t`
  * Holds the kernel ESP saved at the point of preemption; `0` means the process has never been preempted

* `src/kernel/gdt.h` / `src/kernel/gdt.c` — added `tss_get_esp0_ptr()`
  * Returns `&tss.esp0` so `sched_switch.asm` can update it directly without knowing the TSS struct layout

### Changed

* `src/kernel/interrupts.asm` — `irq0_stub` now passes ESP to C (identical pattern to `isr128_stub`)
  * `push esp` before `call irq0_handler_main`; `add esp, 4` after
  * The pushed ESP value points at the full register frame on the kernel stack — the scheduler saves this as the resume point

* `src/kernel/idt.h` / `src/kernel/idt.c` — `irq0_handler_main` signature changed to `void irq0_handler_main(unsigned int esp)`
  * Sends PIC EOI **before** calling `sched_tick` — critical: if `sched_switch` lands on a different context and `irq0_handler_main` never returns through the normal path, the PIC must already be unmasked so future timer ticks fire on the new context
  * `irq0_handler_main` calls `timer_handle_irq()`, sends EOI, then `sched_tick(esp)`

* `src/kernel/kernel.c` — calls `sched_init()` after `idt_init()` and before `sti`

* `src/exec/elf_loader.c`
  * `elf_run_image()` calls `sched_enqueue(proc)` after setting `proc->state = RUNNING` and before `setjmp`/`iret`
  * `elf_process_exit()` calls `sched_dequeue(proc)` before `paging_switch` and `process_destroy`; also restores TSS ESP0 to `0x90000` (static kernel stack top for the shell context)

* `Makefile` — added `sched_switch.o` (asm) and `scheduler.o` (C) to `KERNEL_OBJS` with correct header dependencies

### Design notes

The shell context (slot 0) uses a static `process_t s_shell_proc` with `pd = 0` as a sentinel meaning "kernel PD". Its `sched_esp` is written the first time the timer preempts the shell. Switching back to the shell from a user process goes through `sched_switch` in the normal round-robin path; process *exit* still uses `longjmp` as before, bypassing `sched_switch` entirely.

There is no `sched_yield` syscall yet — processes run until preempted by the timer or until they call `sys_exit`.

---

## [Previous] — process_t abstraction + PD heap leak closed

### Added

* `src/kernel/process.h` / `src/kernel/process.c` — process abstraction layer
  * `process_t` struct consolidates all per-process kernel state:
    * `pd`                  — PMM-allocated page directory pointer
    * `kernel_stack_frame`  — PMM frame used as the ring-0 syscall stack
    * `exit_ctx`            — `jmp_buf` for `setjmp`/`longjmp` exit path
    * `state`               — `process_state_t` enum (`UNUSED`, `RUNNING`, `EXITED`)
    * `name[32]`            — null-terminated process name (truncated to 31 chars)
  * `process_create(name)` — allocates a `process_t` from the PMM (one 4 KB frame),
    zeroes it, sets `state = UNUSED`, copies name; returns pointer or 0 on failure
  * `process_destroy(proc)` — frees `pd` (via `process_pd_destroy`), `kernel_stack_frame`
    (via `pmm_free_frame`), then the `process_t` frame itself; safe on null input
  * `process_set_current(proc)` / `process_get_current()` — track the one running process;
    returns 0 when the kernel is running outside any user process

### Changed

* `src/exec/elf_loader.c` — refactored to use `process_t`
  * Replaced scattered file-scope statics (`s_exit_ctx`, `s_current_pd`,
    `s_kernel_stack_frame`, `s_process_running`) with a single `process_t*` managed
    via `process_get_current()` / `process_set_current()`
  * `elf_run_image()` calls `process_create("elf")`, fills `pd` and
    `kernel_stack_frame`, saves `setjmp` into `proc->exit_ctx`, then launches ring 3
  * `elf_process_exit()` reads `proc->exit_ctx` address before calling
    `process_destroy()` (which frees the containing frame), then `longjmp`s safely
  * `elf_process_running()` now delegates to `process_get_current()` and checks `state`

* `src/kernel/paging.c` — `process_pd_create()` switched from `kmalloc_page()` to
  `pmm_alloc_frame()`
  * The PD is now PMM-allocated and identity-mapped (phys == virt), matching all
    other user-process frames
  * `process_pd_destroy()` now frees the PD frame itself via `pmm_free_frame((u32)pd)`
    as its final step, closing the 4 KB-per-`runelf` heap leak

* `src/kernel/paging.h` — updated `process_pd_create()` and `process_pd_destroy()`
  doc comments to reflect PMM allocation and full cleanup

* `Makefile` — added `process.o` to `KERNEL_OBJS` with correct header dependencies;
  updated `elf_loader.o` rule to declare `process.h` dependency

### Fixed

* **4 KB heap leak per `runelf` invocation** — the process page directory was
  allocated from `kmalloc_page()` (bump allocator, no free) and was never reclaimed.
  Moving PD allocation to the PMM and freeing it in `process_pd_destroy()` closes this.
  `meminfo` should now show an identical free frame count before and after any number
  of `runelf` calls (previously decreased by 1 per call).

### Architecture note

`process_t` is the foundation for the scheduler. When preemptive scheduling is
added, the timer IRQ handler will save register state into the current `process_t`,
pick the next runnable process, and restore its state. The `exit_ctx` field may be
repurposed or supplemented by a full register save area at that point.

---

## [Previous] — SYS_READ + keyboard input buffer

### Added

* `SYS_READ (5)` syscall — blocking keyboard input for ring-3 user programs (`syscall.c`)
  * Reads up to `len` bytes into a user buffer, blocking (STI + HLT loop) until input arrives
  * Echoes each character to the terminal as it is received
  * Terminates early on newline (`\n`)
  * Re-enables interrupts (`sti`) on entry and restores `cli` before returning — required
    because the syscall gate is an interrupt gate (IF cleared on entry by CPU)

* Keyboard input ring buffer — 256-byte circular buffer in `keyboard.c`
  * `keyboard_set_process_mode(1)` redirects ASCII keystrokes into the buffer instead of
    the shell; `keyboard_set_process_mode(0)` restores shell routing and clears the buffer
  * `keyboard_buf_available()` / `keyboard_buf_pop()` — polled by `sys_read_impl`
  * New public API declared in `keyboard.h`

* `s_process_running` flag and `elf_process_running()` in `elf_loader.c`
  * Set to 1 just before `paging_switch` + `iret` into ring 3
  * Cleared to 0 as the first action in `elf_process_exit()`, before PD teardown

* `SYS_READ` wired end-to-end: `uapi_syscall.h` (number) → `syscall.c` (impl) →
  `user_syscall.h` (`sys_read` wrapper) → `user_lib.h` (`u_readline` helper)

* `readline` user program — interactive test for `SYS_READ`
  * Prompts for name and a free-form line, echoes both back with length
  * Added to `USER_PROGS` in Makefile; lives in `src/user/readline.c`

### Fixed

* **IRQ1 permanently blocked during process execution** (`idt.c`)
  * Root cause: `irq1_handler_main` sent the PIC EOI *after* `keyboard_handle_irq` returned.
    When the Enter keypress that launched a process arrived via IRQ1, the call chain
    `keyboard_handle_irq → shell_input_char → shell_execute → elf_run_named → iret`
    never returned — so the EOI was never sent. The 8259 left IRQ1 in-service for the
    entire lifetime of the process, blocking all further keyboard interrupts.
  * Fix: send EOI at the **top** of `irq1_handler_main`, before calling `keyboard_handle_irq`,
    so it is always delivered regardless of whether the handler returns.
  * Note: `irq0_handler_main` (timer) was not affected because `timer_handle_irq` always
    returns; the EOI ordering there is fine either way.

---

## [Previous] — Per-process kernel stacks + PMM frame leak fix

### Added

* Per-process kernel stacks (`elf_loader.c`)
  * Each `runelf` allocates a dedicated 4 KB PMM frame for the kernel stack
  * `tss_set_kernel_stack(frame + PAGE_SIZE)` called before `setjmp` — no window where TSS ESP0 is invalid
  * Frame freed by `elf_process_exit()` via `pmm_free_frame(s_kernel_stack_frame)`
  * Replaces the fragile `esp_now + 64` hack which pointed TSS ESP0 into the live C call frame

### Fixed

* ELF frame leak — user ELFs linked with `-Ttext` placed a PT_LOAD segment at `0x3FF000`
  (PD index 0), which shares the kernel page table. `process_pd_destroy` skipped it as
  a shared kernel entry, leaking one PMM frame per `runelf`. Fixed by switching all user
  program link rules to `-Ttext-segment 0x400000`, which places the entire PT_LOAD segment
  at `0x400000` (PD index 1 — the private ELF region).
* `meminfo` now shows 1536/1536 frames free after any number of `runelf` invocations.

---

## [Previous] — Physical memory manager

### Added
- Added `runelf_test` user program for ELF regression testing
- Added `args` user program for argv/argc validation
- Introduced repeatable user-mode test workflow via `runelf`

### Changed
- Refactored Makefile to use `USER_PROGS` for user ELF builds and ramdisk inclusion
- Ramdisk is now generated dynamically from user program list

### Fixed
- Fixed ELF loader PT_LOAD handling:
  - Correctly handles non-page-aligned segments
  - Properly copies `.data` (file-backed region)
  - Preserves `.bss` zeroing behavior
- Fixed `.data` initialization bug (was always zero)

### Added

* `pmm.h` / `pmm.c` — bitmap-based physical page frame allocator
  * Manages physical frames in the range `0x200000`–`0x7FFFFF` (6 MB, 1536 frames)
  * `pmm_init()` — initialises the bitmap; all frames start free (BSS is pre-zeroed)
  * `pmm_alloc_frame()` — returns a 4 KB-aligned physical address; linear scan from a
    search hint for O(n) worst case; returns 0 and prints a message on exhaustion
  * `pmm_free_frame(addr)` — returns a frame to the pool; detects and warns on double-free
  * `pmm_free_count()` — returns current free frame count; used by `meminfo`
  * 192-byte bitmap in BSS — zero runtime cost, zeroed before `kernel_main` by
    `kernel_entry.asm`

* `terminal_put_uint(value)` / `terminal_put_hex(value)` added to `terminal.h` / `terminal.c`
  * Shared decimal and hex integer printers available to all kernel modules
  * Eliminates the multiple private `static void terminal_put_uint` copies that existed
    in `shell.c`, `commands.c`, and `image_programs.c`

* `memory_get_heap_top()` added to `memory.h` / `memory.c`
  * Exposes the bump allocator's current pointer
  * Used by the `meminfo` shell command

* `meminfo` shell command
  * Prints heap base, top, and used KB
  * Prints PMM free/used/total frame counts and KB equivalents
  * Validates that frames are being reclaimed after `runelf` (used for regression testing)

### Changed

* `process_pd_destroy()` in `paging.c` — no longer a stub
  * Walks all PD entries that differ from the kernel PD (i.e., privately owned by the process)
  * For each private PDE: frees every present PTE's physical frame via `pmm_free_frame()`
  * For PD index 1 (the ELF region): also frees the page table itself via `pmm_free_frame()`
  * Clears each freed PDE to prevent stale CR3 access after destruction

* `paging_map_page()` in `paging.c` — page table allocator split by PD index
  * PD index 1 (ELF region, `USER_PD_INDEX`): page table allocated from PMM so
    `process_pd_destroy()` can free it
  * All other PD indices (including index 767 for the user stack at `0xBFFFF000`):
    page table allocated from `kmalloc_page()` — kernel-owned bookkeeping
  * The physical frames that PTEs point to are always freed by `process_pd_destroy()`
    regardless of which allocator provided the page table

* `elf_loader.c` — user frame allocation moved from bump allocator to PMM
  * ELF segment frames: `pmm_alloc_frame()` instead of `kmalloc_page()`
  * User stack frame: `pmm_alloc_frame()` instead of `kmalloc_page()`
  * These frames are now reclaimed by `process_pd_destroy()` on process exit

* `kernel_main()` — `pmm_init()` called after `memory_init()`

* Physical address space split formalised:

  ```text
  0x100000 – 0x1FFFFF   bump allocator (process PDs, page tables, parse buffers)
  0x200000 – 0x7FFFFF   PMM (user ELF frames, user stack frames)
  ```

  The two ranges are disjoint — `pmm_alloc_frame()` and `kmalloc_page()` can never
  return the same address.

### Fixed

* `process_pd_destroy()` was a stub — physical frames leaked on every `runelf` call.
  The system would exhaust heap memory after repeated executions. Now fully implemented.

* Private `static void terminal_put_uint` in `shell.c` and `image_programs.c` conflicted
  with the new shared declaration in `terminal.h`. Removed from both files.

---

## [Previous] – Ring 3 user mode

### Added

* Ring-3 GDT segments
  * User code descriptor (index 3, DPL=3, selector `0x1B`)
  * User data descriptor (index 4, DPL=3, selector `0x23`)
  * TSS descriptor (index 5, selector `0x28`) — 32-bit available TSS
  * GDT grown from 3 to 6 entries
* TSS (`tss_t`) in `gdt.c`
  * `ss0 = 0x10` (kernel stack segment)
  * `esp0` set per-process via `tss_set_kernel_stack()` before each `iret`
  * `iomap_base = sizeof(tss)` — no I/O permission bitmap
* `tss_set_kernel_stack(esp0)` — updates TSS ESP0; called before every ring-3 launch
* `tss_flush(selector)` in `interrupts.asm` — executes `ltr ax`; called once from `gdt_init()`
* Segment selector constants in `gdt.h`: `SEG_KERNEL_CODE`, `SEG_KERNEL_DATA`, `SEG_USER_CODE`, `SEG_USER_DATA`, `SEG_TSS`
* `elf_process_exit()` in `elf_loader.c`
  * Called by `sys_exit()` from inside the syscall handler
  * Restores kernel CR3, destroys process PD, longjmps back to shell
* Freestanding `setjmp`/`longjmp` (`src/kernel/setjmp.asm`, `src/kernel/setjmp.h`)
  * Saves/restores `ebx`, `esi`, `edi`, `ebp`, `esp`, `eip`
  * Used by `elf_run_image()` to save kernel context before `iret`, restored on `sys_exit`

### Changed

* `elf_run_image()` completely rewritten for ring-3 launch
  * Saves kernel context with `setjmp` before `iret`
  * Sets `TSS.ESP0` to top of dedicated PMM kernel stack frame before switching CR3
  * Launches ELF via `iret` into ring 3 instead of direct function call
  * Returns to shell via `longjmp` triggered by `sys_exit`
* `elf_enter_ring3()` — new internal function
  * Copies argv strings into user stack memory (ring-3 accessible)
  * Builds argv pointer array on user stack
  * Builds cdecl call frame (`argv`, `argc`, fake return address)
  * Loads user data selector into DS/ES/FS/GS
  * Pushes iret frame (SS, ESP, EFLAGS, CS, EIP) and executes `iret`
* `sys_exit()` in `syscall.c` — no longer a stub; calls `elf_process_exit()`
* `int 0x80` IDT gate changed from `IDT_FLAG_INT_GATE_KERNEL` (DPL=0) to `IDT_FLAG_INT_GATE_USER` (DPL=3)

### Fixed

* argv strings were in kernel heap memory — inaccessible to ring-3 code
* Programs previously ran in ring 0 with no privilege enforcement

---

## Per-process page directories

### Added

* Per-process page directories (`process_pd_create`, `process_pd_destroy`)
* `kmalloc_page()` in `memory.c`
* `paging_switch(pd)`, `paging_get_kernel_pd()`
* User stack allocation per process — one page at `USER_STACK_TOP - PAGE_SIZE`

### Changed

* `elf_loader.c` rewritten to use per-process page directories
* All user ELFs linked at `0x400000`
* `paging_map_page()` signature changed: first argument is now `u32* pd`

### Fixed

* User programs previously ran in the kernel's flat address space with no isolation

---

## Paging, ramdisk, LBA boot

### Added

* x86 paging — identity-maps first 8 MB
* Ramdisk — flat ELF archive loaded from disk, no kernel rebuild to add programs
* LBA extended disk reads in loader2 (INT 0x13 AH=0x42)
* BSS zeroing in `kernel_entry.asm`
* `mkramdisk` build tool (C)

### Fixed

* Triple fault on boot — BSS not zeroed before `paging_init()`
* Disk read error — CHS limit, real-mode 1MB limit, sector alignment
* "ramdisk: bad magic" — all three root causes resolved

---

## Syscalls, GDT stabilization, project reorganization

### Added

* Kernel-owned GDT
* Syscall interface (`int 0x80`): `SYS_WRITE`, `SYS_EXIT`, `SYS_GET_TICKS`, `SYS_PUTC`
* Userspace syscall wrapper (`user_syscall.h`)
* ELF execution via syscalls
* Structured project layout

### Fixed

* Triple fault after `sti` — kernel GDT not installed
* Syscall breakage — `syscall_regs_t` mismatch with ISR stack layout

---

## ELF loader and shell stabilization

### Added

* ELF loader, `runelf` command
* `runimg` execution layer
* Line editor, command parser, command dispatcher

---

## [Initial] – Core system bring-up

### Added

* Bootloader (real → protected mode)
* Kernel entry and initialization
* IDT, PIC remap, timer (PIT), keyboard driver
* VGA text output, terminal abstraction, basic shell

---

## Future Milestones

* `SYS_READ` + keyboard input buffer for user programs
* Process abstraction (`process_t` struct, process table)
* Preemptive scheduler (timer IRQ triggers context switch)
* Filesystem-backed ELF loading (FAT12 or custom FS via ATA PIO)