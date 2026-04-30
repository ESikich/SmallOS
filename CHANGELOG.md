# Changelog

## [Current] — Make-time selftest + loader fallback + dynamic help

### Added

* **Make-time ELF selftest** (`Makefile`, `tools/qemu_selftest.py`, `src/shell/commands.c`)
  * `make test` boots the image headlessly, launches the shell `selftest` command, feeds the interactive `readline` prompt, and checks every shipped ELF
  * `selftest` runs the full built-in shell command suite and shipped-program matrix from the kernel side, including the fault cases
  * Per-command expectation files now live under `tests/shell/`, and per-ELF expectation files live under `tests/elfs/`, so the suite is split into small, readable modules
  * The host-side QEMU monitor helper watches the serial log and shuts the VM down after the suite reports `PASS`

* **Dynamic `help` output** (`src/shell/commands.c`)
  * Built-in shell commands now render from the actual command table with short descriptions
  * Shipped ELF programs now render from a program table with the same short descriptions
  * The `help` text stays in sync when commands or programs change, instead of being hand-written prose

* **ELF name fallback** (`src/exec/elf_loader.c`)
  * `runelf` / `SYS_EXEC` now accept bare program names and fall back to `name.elf` when needed
  * That keeps the shell examples ergonomic while still loading the real FAT16 `*.ELF` files

* **Permanent fault probe ELF** (`src/user/fault.c`)
  * `fault.elf` provides a repeatable way to trigger `#UD`, `#GP`, `#DE`, `#BR`, and `#PF`
  * Default mode is `ud` so a bare `runelf fault.elf` still exercises the exception path

* **Exception hardening** (`src/kernel/idt.c`, `src/kernel/interrupts.asm`)
  * `#UD`, `#GP`, `#DE`, `#BR`, and `#PF` now log consistent crash records
  * User faults terminate only the offending process and return to the shell
  * Kernel faults still halt so the last error context is preserved
  * `#DF` has a visible `DF!` emergency marker before halting

* **Help and docs refresh**
  * README and build/development docs now describe the built-in command table and shipped program table
  * README and build docs now mention `make test` and the shell `selftest` command
  * Windows / PowerShell debug launch documented with GTK and QEMU logging flags
  * Boot/layout docs updated for the generated stage-2 stack guard and the `0xB000` loader2 placement

* **Process exit status plumbing** (`src/kernel/process.c`, `src/kernel/syscall.c`, `src/kernel/idt.c`)
  * Foreground `runelf` calls now preserve the child exit status
  * The shell selftest uses that status to verify normal exits and expected fault terminations

### Changed

* **`reboot` command documentation** — the shell help now labels `reboot` as rebooting the machine, and the docs note the Windows/QEMU debug launch that preserves normal reboot behavior

### Fixed

* **Stale program lists in docs** — the README and build docs now include `fault.elf` and the rest of the shipped ELF catalog
* **Shell help drift** — the help text now reflects the actual built-in command and ELF program tables instead of a hard-coded list

---

## [Previous] — SYS_OPEN / SYS_CLOSE / SYS_FREAD + copy-from-user validation
 
### Added
 
* **Per-process file descriptor table** (`src/kernel/process.h`)
  * `fd_entry_t` struct: `valid`, `name[16]`, `size`, `offset`
  * `fds[PROCESS_FD_MAX]` (8 slots) embedded in `process_t` — zero-initialized by `proc_zero()` in `process_create()`; freed automatically with the process frame; no explicit close-on-exit needed
  * fds 0/1/2 reserved by convention; user-opened files start at fd 3 (`PROCESS_FD_FIRST`)
 
* **`SYS_OPEN (8)`** — open a FAT16 file by name
  * Validates name pointer with `user_str_ok()`, copies into a kernel buffer (bounded by `PROCESS_FD_NAME_MAX = 16`)
  * Calls `fat16_stat()` to confirm existence without loading data
  * Allocates lowest free fd slot; stores name, size, offset=0
  * Returns fd ≥ 3 on success, -1 on failure
 
* **`SYS_CLOSE (9)`** — close an fd, freeing its slot for reuse
 
* **`SYS_FREAD (10)`** — read bytes from an open fd into a user buffer
  * Validates user buffer with `user_buf_ok()`
  * Calls `fat16_load()` to load the full file, copies the slice `[offset, offset+len)` into the user buffer, advances offset
  * Returns bytes copied, 0 at EOF, -1 on error
 
* **`fat16_stat(name, out_size)`** (`src/drivers/fat16.h` / `fat16.c`)
  * Walks the root directory and returns file size without touching the static load buffer
  * Used by `SYS_OPEN` to validate existence cheaply
 
* **Copy-from-user validation** (`src/kernel/syscall.c`)
  * `user_buf_ok(ptr, len)` — rejects any buffer not entirely within `[USER_CODE_BASE, USER_STACK_TOP)` = `[0x400000, 0xC0000000)`; overflow-safe
  * `user_str_ok(ptr)` — validates string pointer base only (length unknown before scan)
  * Applied to all pointer-bearing syscalls: `SYS_WRITE`, `SYS_READ`, `SYS_EXEC` (name + argv array), `SYS_OPEN`, `SYS_FREAD`
 
* **`src/user/fileread.c`** — end-to-end test program
  * Opens itself (`fileread.elf`), reads and hex-dumps first 16 bytes (ELF magic), drains to EOF, closes fd
  * Verifies double-close and bad-fd both return -1
 
### Key design notes
 
* **The fd table lives inside `process_t`** — no separate allocation, no leak risk, destroyed automatically with the process.
* **`SYS_FREAD` reloads from ATA on every call** — intentionally simple for now; caching is a future optimization.
* **`fat16_stat` does not disturb `s_load_buf`** — `SYS_OPEN` validation and ELF loading can interleave safely.
* **Copy-from-user is address-range only** — it does not walk page tables. A user pointer in the valid range but backed by a missing PTE would still fault in the kernel; full fault handling is future work.
 
---
 
## [Previous] — True blocking SYS_READ
 
### Changed
 
* **`src/kernel/syscall.c`** — `sys_read_impl` replaced busy-wait with scheduler-aware blocking
  * When `kb_buf` is empty: sets `proc->state = PROCESS_STATE_WAITING`, registers the process via `keyboard_set_waiting_process(proc)`, and executes `hlt`
  * The timer IRQ fires normally; `sched_tick` skips `WAITING` tasks so other processes run
  * When a keypress arrives, `process_key_consumer()` wakes the waiter by setting its state back to `PROCESS_STATE_RUNNING` and clearing the waiter slot
  * The process resumes inside `sys_read_impl` after the `hlt`, re-checks `keyboard_buf_available()`, and continues draining the buffer
  * `sched_yield_now()` is **not** called from `sys_read_impl` — calling it with the `isr128_stub` resume ESP would bypass the C stack entirely and resume the process in ring-3 user space, abandoning the read loop
 
* **`src/kernel/process.c`** — `process_key_consumer` now wakes the parked process after pushing to `kb_buf`
  * After `keyboard_buf_push_char(c)`, checks `keyboard_get_waiting_process()` and flips state to `RUNNING` if a waiter is present
  * `process_set_foreground(proc)` now calls `keyboard_buf_clear()` before switching the consumer, discarding any stale input (e.g. the Enter keypress that launched `runelf`) that would otherwise pre-fill the first `sys_read` call
  * `process_destroy()` clears the waiter slot if the dying process is the registered waiter, preventing a dangling pointer
 
* **`src/kernel/process.h`** — added `PROCESS_STATE_WAITING = 4` to `process_state_t`
 
* **`src/drivers/keyboard.c`** / **`src/drivers/keyboard.h`**
  * Added `s_waiting_proc` slot and public accessors `keyboard_set_waiting_process()` / `keyboard_get_waiting_process()`
  * `kb_buf_clear()` promoted to public API as `keyboard_buf_clear()`
  * `keyboard_init()` now clears the waiter slot and calls `keyboard_buf_clear()`
 
### Fixed
 
* **Spurious newline after every typed character** — `sched_yield_now(resume_esp)` was called from inside `sys_read_impl` with the `isr128_stub` frame ESP. This caused the scheduler to resume the process directly in ring-3 user space (via `iretd`), abandoning `sys_read_impl`'s C stack. The process re-entered user code as if `sys_read` had returned with garbage in `eax` after every single keypress. Fixed by using `PROCESS_STATE_WAITING` + `hlt` instead of `sched_yield_now`.
 
* **Stale Enter consumed as first input character** — the `'\n'` from the Enter keypress that launched `runelf` was sitting in `kb_buf` when the process first called `sys_read`, causing it to return an empty line immediately. Fixed by calling `keyboard_buf_clear()` in `process_set_foreground()` when handing the consumer to a process.
 
### Key design notes
 
* **`PROCESS_STATE_WAITING` is skipped by `sched_find_next_runnable_from()`** — no change to the scheduler was needed; it already checks `state == PROCESS_STATE_RUNNING`.
* **Wake-up happens in IRQ1 context** — `process_key_consumer` runs with IF=0. The only work it does is a state-flag write and a pointer clear — safe and non-blocking.
* **Single waiter slot** — matches the single-foreground-process model. Only the foreground process can be in `PROCESS_STATE_WAITING` at any time.
 
---
 
## [Previous] — Zombie reaper kernel task
 
### Added
 
* **Zombie reaper** — permanent kernel task that automatically reclaims processes with no explicit waiter
  * `sched_reap_zombies()` in `scheduler.c` — walks `s_table` back-to-front, calls `process_destroy()` on every `PROCESS_STATE_ZOMBIE` task that is not the currently running task
  * `reaper_task_main()` in `process.c` — kernel task loop: call `sched_reap_zombies()`, then `sti; hlt` until the next timer tick
  * `process_start_reaper()` — creates and enqueues the reaper; called from `kernel_main()` before `sched_start()`
 
### Fixed
 
* `runelf_nowait` and `SYS_EXEC` children that exited with no waiter leaked their `process_t` frame and kernel stack frame permanently. The reaper now frees them within one scheduler quantum.
 
### Key design notes
 
* The reaper sleeps in `hlt` between scans — CPU overhead is negligible.
* `sched_reap_zombies()` skips the currently running slot, preserving the invariant that a task's kernel stack must not be freed while it is still executing.
* `process_wait()` remains the preferred path for foreground children; the reaper only handles unclaimed zombies.
 
---
 
## [Previous] — SmallOS cleanup pass

### Added

* **`src/kernel/klib.c` / `src/kernel/klib.h`** — shared freestanding kernel utility library
  * consolidates shared string / memory helpers previously duplicated across multiple files
  * exports `k_memcpy`, `k_memset`, `k_strlen`, `k_strcmp`, `k_strncpy`, and `k_starts_with`
  * `Makefile` now builds `klib.o` as part of `KERNEL_OBJS`

### Changed

* **`src/exec/elf_loader.c` / `src/exec/elf_loader.h`**
  * removed dead `elf_process_running()` declarations/implementation from the old foreground-ELF model
  * cleaned include paths to match the rest of the tree, while intentionally keeping `../kernel/elf.h` relative because that header lives under `src/kernel/`
  * `tss_set_kernel_stack()` is no longer called during `elf_run_image()` setup
  * first-process-entry ESP0 setup now happens in `elf_user_task_bootstrap()`, which avoids clobbering the currently running task's TSS ESP0 during async launches such as `runelf_nowait` and `SYS_EXEC`

* **`src/shell/shell.c`**
  * shell task idle loop no longer busy-spins when no keyboard events are queued
  * `shell_task_main()` now executes `hlt` while idle and wakes on the next timer tick or keyboard IRQ

* **`src/kernel/scheduler.c` / `src/kernel/memory.h`**
  * replaced the bare `0x90000u` boot-stack fallback with named constant `KERNEL_BOOT_STACK_TOP`

* **Utility cleanup across the tree**
  * removed now-redundant local string / memory helpers from `elf_loader.c`, `process.c`, `shell.c`, `commands.c`, and `paging.c`

### Removed

* **Private copies of shared string / memory helpers** that are now centralized in `klib`
* **Dead shell helper** `str_starts_with`
* **Dead ELF state query helper** `elf_process_running()`

### Key design notes

* **Shared freestanding primitives now have one canonical home.**
  New general-purpose string and memory helpers should go in `klib`, not as file-local statics.

* **TSS.ESP0 must track the task that will actually return from ring 3 next.**
  Updating it in `elf_user_task_bootstrap()` preserves correctness for asynchronous process creation.

---

## [Previous] — ELF execution promoted to scheduler-owned user tasks

### Added

* **Foreground wait/reap path for scheduler-owned ELF tasks**
  * `process_wait(proc)` — blocks in a `sti; hlt` loop until the child reaches `PROCESS_STATE_ZOMBIE`, then destroys it from a safe stack
  * `runelf_nowait <name> [args]` shell command — enqueues an ELF task and returns immediately
  * foreground input ownership in `process.c`:
    * `process_set_foreground(proc)`
    * `process_get_foreground()`

### Changed

* **`src/exec/elf_loader.c`**
  * `elf_run_image()` no longer uses the old foreground `setjmp`/`longjmp` path
  * now:
    * creates the process
    * builds the process page directory and mappings
    * allocates a per-process kernel stack
    * seeds `proc->sched_esp` with `elf_user_task_bootstrap`
    * marks the task `PROCESS_STATE_RUNNING`
    * enqueues it with `sched_enqueue(proc)`
    * returns `process_t*`
  * `elf_enter_ring3()` now sets IF in the pushed EFLAGS before `iret`

* **`src/kernel/syscall.c`**
  * `SYS_EXIT` is now scheduler-owned:
    * switches to the kernel page directory
    * calls `sched_exit_current((unsigned int)regs)`
    * no longer returns via `elf_process_exit()` / `longjmp`
  * `SYS_EXEC` is now async spawn:
    * copies `name` into a kernel buffer
    * calls `elf_run_named()`
    * returns immediately with `0` / `-1`
  * `SYS_YIELD` now passes the correct scheduler resume base (`esp - 8`), matching the IRQ0/syscall stub contract

* **`src/kernel/scheduler.c` / `src/kernel/scheduler.h`**
  * `sched_exit_current()` now:
    * marks the current task `PROCESS_STATE_ZOMBIE`
    * dequeues it
    * switches directly to the next runnable task
    * does **not** destroy the task there
  * scheduler save/restore path corrected so task `sched_esp` preserves the real interrupt/syscall resume frame instead of being clobbered by the scheduler's own C call-frame ESP
  * timer path now refreshes the shell task's saved `sched_esp` even when it is the only runnable task, so later switches back to the shell resume a real preempted context instead of the stale bootstrap stack

* **`src/kernel/process.h` / `src/kernel/process.c`**
  * added `PROCESS_STATE_ZOMBIE`
  * removed the old hybrid foreground override model
  * `process_get_current()` now returns `sched_current()`
  * `process_t` no longer needs foreground `setjmp`/`longjmp` execution semantics
  * foreground terminal/input ownership is now tracked explicitly and used by blocking `runelf`

* **`src/shell/commands.c`**
  * `runelf` now behaves like the old foreground path from the user's perspective by spawning an ELF task and waiting in `process_wait(proc)`
  * `runelf_nowait` added as the explicit async spawn path

* **`src/drivers/keyboard.c` / `src/drivers/keyboard.h`**
  * removed the old global keyboard “process mode” routing model
  * input routing now uses:
    1. foreground user process owner, if any
    2. otherwise the currently scheduled user task
    3. otherwise the shell/editor
  * this fixes input races for foreground interactive programs such as `readline`

### Removed

* **Old foreground ELF execution path**
  * foreground `setjmp` / `longjmp` launch/return model
  * `elf_process_exit()`
  * parent-tracking statics such as `s_parent_proc` / `s_parent_esp0`
  * `process_set_current()` foreground override semantics
  * global keyboard process-mode switching (`keyboard_set_process_mode`)

### Key design notes

* **ELF programs are now real scheduler-owned tasks.**
  `runelf` preserves old user-visible blocking behavior by waiting on a scheduled child, not by directly foreground-jumping into ring 3.

* **Exit is split into two phases by design.**
  `SYS_EXIT` only transitions the task to `ZOMBIE` and switches away. The actual destroy happens later from a safe stack (`process_wait()`), avoiding freeing the currently executing kernel stack.

* **Foreground interactive correctness now depends on foreground ownership, not just `sched_current()`.**
  This prevents the shell from stealing keystrokes while it is blocked waiting for a foreground child.

* **Background tasks are scheduler-owned but not yet automatically reaped.**
  Foreground runs are cleaned up by `process_wait()`. Async/background runs still need a future reap model if long-lived background execution becomes a first-class feature.

## [Previous] — Remove run/runimg/exec infrastructure

### Removed

* **`src/exec/programs.c` / `src/exec/programs.h`** — kernel-linked builtin programs. Superseded by ELF execution.
* **`src/exec/images.c` / `src/exec/images.h`** — descriptor-based image execution. Superseded by ELF execution.
* **`src/exec/image_programs.c`** — image implementations (`img_hello_main`, `img_args_main`).
* **`src/exec/exec.c` / `src/exec/exec.h`** — `exec_run_entry()` wrapper. No longer called.
* **`run` shell command** — called `programs_run()`.
* **`runimg` shell command** — called `images_run()`.

### Changed

* **`src/shell/commands.c`** — removed `#include "programs.h"`, `#include "images.h"`, `cmd_run()`, `cmd_runimg()`, their command table entries, and their help text lines.
* **`Makefile`** — removed `programs.o`, `exec.o`, `images.o`, `image_programs.o` from `KERNEL_OBJS` and their build rules.

`runelf` is now the only program execution path. `src/exec/` contains only `elf_loader.c/h`.

---

## [Previous] — Ramdisk removal

### Removed

* **`src/kernel/ramdisk.c` / `src/kernel/ramdisk.h`** — deleted entirely. `ramdisk_find()` was the only consumer and has been replaced by `fat16_load()`.
* **`tools/mkramdisk.c`** — deleted. The ramdisk is no longer built.
* **Ramdisk loading from `loader2.asm`** — `load_ramdisk`, `RAMDISK_SECTORS`, `RAMDISK_LBA`, `RAMDISK_TMP_SEG/OFF`, ramdisk debug print, and `ramdisk_loaded_msg` all removed. Loader2 now loads only the kernel.
* **Ramdisk from `kernel.c`** — `#include "ramdisk.h"` and `ramdisk_init()` removed.
* **Ramdisk from Makefile** — `ramdisk.o` from `KERNEL_OBJS`, `RAMDISK_ENTRIES` variable, `mkramdisk` tool rule, `ramdisk.rd` build rule, ramdisk padding logic in `os-image.bin`, and the ramdisk term from the FAT16 LBA calculation all removed.

### Changed

* **`loader2.asm`** — now only loads the kernel. `__KERNEL_SECTORS__` is the only injected placeholder. Boot sequence prints `LBA ok Loading...K` then enters protected mode.

* **`Makefile` `loader2.gen.asm` rule** — simplified; only injects `__KERNEL_SECTORS__`.

* **`Makefile` `os-image.bin` rule** — disk image is now `boot + loader2 + kernel_padded + fat16.img`. FAT16 LBA = `5 + kernel_sectors` (no ramdisk term).

* **Disk image layout** (final):
  ```text
  LBA 0         boot.bin              (512 bytes)
  LBA 1–4       loader2.bin           (2048 bytes)
  LBA 5+        kernel_padded.bin     (sector-aligned)
  LBA 5+ks      fat16.img             (16 MB FAT16 partition)
  ```
  where `ks = ceil(kernel.bin / 512)`.

### Key design note

The ramdisk served as a temporary program store while the FAT16 driver was being built. With `fat16_load()` proven and `elf_run_named()` using it, the ramdisk was entirely redundant. Removing it simplifies the boot path, shrinks the disk image by ~13 KB, and eliminates the loader2 overlap risk from the ramdisk padding calculation.

---

## [Previous] — FAT16 parser, ELF loading from disk, loader2 relocation

### Added

* **`src/drivers/fat16.c` / `src/drivers/fat16.h`** — read-only FAT16 filesystem driver
  * `fat16_init(lba)` — reads the FAT16 boot sector at the given absolute LBA, validates the BPB geometry against compile-time constants
  * `fat16_ls()` — prints all non-empty root directory entries (name, size, cluster)
  * `fat16_load(name, out_size)` — finds a file by case-insensitive 8.3 name, follows the FAT16 cluster chain, loads the full file into a static buffer, returns a pointer
  * No `kmalloc` — uses static BSS buffers (`s_load_buf[256 KB]`, `s_cluster_buf[2 KB]`, `s_sector[512]`) so heap stays flat across all calls
  * `fat16_load` is safe to call repeatedly — `elf_run_image` copies data into PMM frames before returning, so the static buffer is reused cleanly

* **`fsls` shell command** — calls `fat16_ls()`, lists FAT16 root directory
* **`fsread <n>` shell command** — calls `fat16_load()`, dumps first 16 bytes; used to verify ELF magic (`7F 45 4C 46`) before trusting FAT16 loading

### Changed

* **`src/exec/elf_loader.c`** — `elf_run_named()` now calls `fat16_load()` instead of `ramdisk_find()`. ELF programs are loaded from the FAT16 partition on disk. Ramdisk is no longer used for ELF execution (still loaded by loader2 but now dead code pending removal).

* **`src/boot/boot.asm`** — loader2 load address changed from `0x8000` to `0xA000`

* **`src/boot/loader2.asm`** — origin changed from `0x8000` to `0xA000`

  **Root cause:** kernel grew beyond 56 sectors (~29 KB), causing the kernel LBA read (physical `0x1000`) to overwrite loader2 at `0x8000` while the BIOS INT 0x13 transfer was still in progress. The read hung silently mid-transfer.

  Safe kernel ceiling with loader2 at `0xA000`: `(0xA000 - 0x1000) / 512 = 72 sectors = 36 KB`. If the kernel exceeds this, move loader2 to `0xB000`.

* **`kernel.c`** — `fat16_init(FAT16_LBA)` called after `ata_init()`, before shell task creation

* **`Makefile`**
  * `fat16_lba.h` generated as a standalone rule from `kernel.bin` and `ramdisk.rd` sizes; only `kernel.c` includes it — `fat16.c` has no generated-header dependency
  * FAT16 LBA patched into boot sector offset 504 as a little-endian u32 using `dd conv=notrunc` after image assembly
  * `elf_loader.o` dependency updated from `ramdisk.h` to `fat16.h`
  * `fat16.o` dependency on `memory.h` removed
  * `boot.asm` and `loader2.asm` updated for new loader2 address

### Key design notes

**Static load buffer:** `fat16_load` writes into `s_load_buf[FAT16_MAX_FILE_BYTES]` (256 KB BSS). ELF programs run sequentially in the foreground so the buffer is never aliased. `elf_run_image` copies all segment data into PMM frames during loading, so the buffer is safe to reuse. Heap stays flat across all `runelf` calls.

**Loader2 relocation invariant:** `loader2_address - 0x1000 > kernel_sectors * 512`. At `0xA000` this permits kernels up to 72 sectors.

**FAT16 LBA delivery:** `fat16_init(lba)` takes the LBA as a parameter passed from `kernel_main()`. `fat16.c` has no generated-header dependency.

---

## [Previous] — ATA PIO driver + FAT16 disk image

### Added

* **`src/drivers/ata.c` / `src/drivers/ata.h`** — ATA PIO driver (primary channel, master drive)
  * `ata_init()` — software reset via device control register, polls BSY until ready
  * `ata_read_sectors(lba, count, buf)` — 28-bit LBA polling read; loads task-file registers, issues `READ SECTORS (0x20)`, reads 256 16-bit words per sector via `inw`; returns 1 on success, 0 on ERR/DF/timeout
  * No DMA, no IRQ — pure polling; QEMU IDE controller emulates the primary channel at `0x1F0`
  * `inw` issued inline since `ports.h` only provides `inb`/`outb`

* **`tools/mkfat16.c`** — host tool that builds a raw FAT16 disk image with no external dependencies (no `mkfs.vfat`, no `mtools`)
  * Fixed 16 MB volume (32768 × 512-byte sectors)
  * Layout: 4 reserved sectors, 2 × 32-sector FATs, 32-sector root directory (512 entries), data region from sector 100
  * Writes BPB, both FAT copies, root directory entries, and file data entirely in C
  * Filenames converted to 8.3 uppercase format
  * Built by host `gcc` into `build/tools/mkfat16`

* **`build/gen/fat16_lba.h`** — generated header with `FAT16_LBA` and `FAT16_SECTORS` as compile-time constants; written by the `os-image.bin` Makefile rule so kernel C code can `#include "fat16_lba.h"` without any runtime detection

* **`ataread <lba>` shell command** — dumps the first 32 bytes of any sector as hex; also prints the boot signature bytes at offsets 510–511 when LBA 0 is requested. Used to verify ATA reads and FAT16 placement.

### Changed

* **Makefile**
  * Added `$(OBJ_DIR)/ata.o` to `KERNEL_OBJS`
  * Added `$(TOOLS_DIR)/mkfat16` host tool rule
  * Added `$(BIN_DIR)/fat16.img` rule — calls `mkfat16` with all user ELFs
  * `os-image.bin` rule now pads **both** `kernel.bin` and `ramdisk.rd` to 512-byte sector boundaries before `cat`; previously only the kernel was padded, which caused the FAT16 LBA calculation to be off by up to 511 bytes
  * `os-image.bin` rule appends `fat16.img` after the padded ramdisk and writes `fat16_lba.h`
  * Added `-I$(GEN_DIR)` to `CFLAGS` so generated headers are findable
  * Added `exec_test` to `USER_PROGS`

* **Disk image layout** — now includes FAT16 partition after the ramdisk:
  ```text
  LBA 0         boot.bin              (512 bytes)
  LBA 1–4       loader2.bin           (2048 bytes)
  LBA 5+        kernel_padded.bin     (sector-aligned)
  after kernel  ramdisk_padded.rd     (sector-aligned, temporary)
  after ramdisk fat16.img             (16 MB FAT16 volume)
  ```

### Key design notes

**Ramdisk sector padding:** The ramdisk is a flat binary with no alignment guarantee. Before this fix, `cat` appended the FAT16 image starting mid-sector, so `FAT16_LBA` was computed correctly but the data on disk was offset by the ramdisk's fractional sector. Both binary blobs must be padded to sector boundaries before concatenation.

**No external FS tools:** `mkfat16` writes the FAT16 BPB, FAT tables, and directory entries directly. The layout constants (`DATA_START = 100`, `ROOT_DIR_SECTORS = 32`, `FAT_SECTORS = 32`) are verified against the actual on-disk byte offsets — cluster 2 maps to sector 100, confirmed by finding ELF magic bytes at `ataread <FAT16_LBA + 100>`.

**`FAT16_LBA` is a compile-time constant**, not detected at runtime. It is recomputed and the header regenerated on every `make` so it stays in sync with the actual image layout.

---

## [Previous] — SYS_YIELD + SYS_EXEC

### Added

* **`SYS_YIELD` (syscall 6)** — voluntary preemption
  * `uapi_syscall.h` — added `SYS_YIELD = 6`
  * `user_syscall.h` — added `sys_yield()` inline wrapper using `syscall0`
  * `scheduler.c` / `scheduler.h`
    * Extracted shared switch core into `sched_do_switch(esp)` — used by both `sched_tick` and `sched_yield_now`
    * Added `sched_yield_now(esp)`: resets `s_tick_count` to 0 and calls `sched_do_switch(esp)`, bypassing the quantum guard
  * `syscall.c` — `sys_yield_impl(esp)` receives `(unsigned int)regs` and calls `sched_yield_now(esp)`; the `isr128_stub` frame is structurally identical to `irq0_stub` so the resume point is valid for `sched_switch`
  * `yield_test.c` — new ramdisk program: calls `sys_yield()` in a counted loop, prints tick-stamped output before and after each yield

* **`SYS_EXEC` (syscall 7)** — user process spawns a named child ELF
  * `uapi_syscall.h` — added `SYS_EXEC = 7`
  * `user_syscall.h` — added `sys_exec(name, argc, argv)` inline wrapper using `syscall3`
  * `syscall.c` — added `sys_exec_impl(name, argc, argv)`: copies name from user address space to a kernel stack buffer (required because `ramdisk_find` runs after CR3 switches to child's PD), then calls `elf_run_named()`
  * `elf_loader.c` — parent context save/restore:
    * Added `s_parent_proc` and `s_parent_esp0` statics
    * `elf_run_image()` saves `process_get_current()` and the parent's `kernel_stack_frame + PAGE_SIZE` (or `0x90000` for the shell) before installing the child
    * `elf_process_exit()` switches CR3 to `s_parent_proc->pd` (not the kernel PD) before `longjmp` — required because the parent's ring-3 EIP at `0x400000` is only mapped in the parent's page directory; switching to the kernel PD first causes an immediate page fault on `iretd`
    * `elf_process_exit()` restores TSS ESP0 to `s_parent_esp0` and calls `process_set_current(s_parent_proc)`
    * `elf_enter_ring3()` now uses `proc->user_argc` / `proc->user_argv` (kernel-side copies) instead of the raw `argc`/`argv` — the raw pointers may be from user space and are invalid after CR3 switches
  * `exec_test.c` — new ramdisk program: calls `sys_exec("hello", ...)`, verifies control returns, tests a nonexistent name returns -1, exits cleanly

### Key design notes

**SYS_YIELD frame compatibility:** The `int 0x80` gate builds the same register frame layout as `irq0_stub` (pusha + 4 segment pushes + esp push). Passing `regs` directly to `sched_yield_now` as the resume ESP is therefore safe — no special frame construction needed.

**SYS_EXEC CR3 rule:** `elf_process_exit` must switch to the parent's page directory, not the kernel PD, before `longjmp`. The `longjmp` itself traverses kernel code (visible in all PDs), but the `iretd` at the bottom of `isr128_stub` jumps to the parent's ring-3 EIP at `0x400000` — only mapped in the parent's PD.

**argv pre-copy:** All argv strings must be in `process_t.user_arg_data` before CR3 switches. `elf_enter_ring3` must use `proc->user_argv` (kernel-side pointers), not the original `argv` argument.

---

## [Previous]

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
