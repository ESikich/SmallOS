# Build System

This document defines how the system is built, how artifacts are generated, and how the disk image is constructed.

---

# Overview

The build process produces:

```text
build/img/os-image.bin
```

This image contains:

```text
boot.bin         stage 1 bootloader   (size declared by boot.asm; currently 512 bytes)
loader2.bin      stage 2 loader       (size declared by loader2.asm; currently 2048 bytes)
kernel.bin       kernel               (padded to sector boundary during final image assembly)
fat16.img        FAT16 partition      (fixed-size FAT volume, stored after the padded kernel)
```

---

# Toolchain

```text
nasm             → assembly (boot stages, interrupt stubs, kernel entry)
i686-elf-gcc     → C compilation (freestanding, 32-bit, no stdlib)
i686-elf-ld      → linking
i686-elf-objcopy → strip ELF metadata → flat binary
gcc              → host tool compilation (mkfat16, mkimage)
```

---

# Build Output Structure

```text
build/
├── bin/   → final binaries (kernel.elf, kernel.bin,
│             apps/demo/hello.elf, apps/tests/*.elf,
│             fat16.img, boot.bin, loader2.bin, tcc-smalos.elf)
├── obj/   → object files and depfiles (.o, .d), mirrored by source subtree
├── gen/   → generated source (loader2.gen.asm)
├── img/   → final disk image (os-image.bin)
└── tools/ → host tools (mkfat16, mkimage)
```

---

# High-Level Build Flow

```text
source files (.c, .asm)
  ↓
object files (.o)
  ↓
kernel.elf          apps/demo/hello.elf   apps/tests/*.elf   ...
  ↓                      ↓
kernel.bin         user program ELFs
  ↓                      ↓
loader2.gen.asm     fat16.img           ← built by build/tools/mkfat16 from all user ELFs
  ↓                      ↓
loader2.bin          boot.bin
          \            |            /
           \           |           /
            \          |          /
                 mkimage
                   ↓
              os-image.bin
```

`mkimage` performs the final disk-image assembly step. It pads `kernel.bin` to a whole number of sectors, computes the FAT16 start LBA, concatenates the component binaries, and patches the FAT16 start LBA into the boot-sector field declared by `boot.asm`.

`make boot-layout-check` verifies the generated boot-chain inputs before that step runs, and `make image-layout-check` verifies the finished `os-image.bin` afterwards.

`make verify` is the one-shot preflight target: it runs both layout checks, then `make test`, then `make smoke`.

## Automated Guest Test

`make test` boots the finished image headlessly, launches the shell
`selftest` command, feeds the interactive `readline` prompt, and
verifies the built-in shell command suite in `tests/shell/` plus every
shipped ELF against the per-program expectation files in `tests/elfs/`.

The scripted shell/selftest command tables are stored statically in the
kernel because the shell task's kernel stack is only 4 KB wide. That keeps
the long regression run from corrupting process state while the guest test
harness is exercising the full matrix.

The guest suite also exercises the SmallOS-hosted TinyCC compiler
(`tools/tcc.elf`) by compiling several sample C files inside the guest
and immediately running the generated ELFs. `tools/tcc.elf` is driven by a
SmallOS-side wrapper around libtcc so it can run cleanly in the freestanding
guest runtime. Those generated binaries are stored under `apps/tests/`, while
the shipped hello demo lives under `apps/demo/`.

The user runtime behavior that those tests depend on is documented in
[`docs/user-runtime.md`](user-runtime.md), including `errno`, cwd-aware
wrappers, stdio, directory traversal, and TinyCC expectations.

`make smoke` runs the dedicated reboot and halt smoke checks.  Use
`make smoke-reboot` or `make smoke-halt` to exercise those shell
commands on their own.

`make ftp-smoke` boots QEMU with user-network host forwarding for FTP control
port `2121` and passive data port `30000`, starts `apps/services/ftpd`, then
drives `LIST`, `RETR`, and `STOR` from the host.

For networking, the default `run` and `test` targets keep using QEMU's
user-network NAT so CI stays simple. `make run-tap` and
`make run-headless-tap` switch the e1000 NIC over to a host TAP device
instead. That is the right path when you want the guest on a bridged LAN
or otherwise reachable beyond QEMU's built-in NAT layer.

For interactive display/input, `make run` defaults to QEMU's curses backend:

```bash
make run
```

If curses feels laggy through WSL, Windows Terminal, or another terminal
bridge, use a graphical QEMU backend instead:

```bash
make run-gtk
make run-sdl
make run QEMU_DISPLAY=gtk
```

On Windows, TAP networking requires an additional TAP driver. The QEMU
documentation notes that the TAP-Win32 driver is not bundled with standard
QEMU for Windows and must be installed separately. If you are not setting
that up, stay with the default user-network mode.

---

# Kernel Build

## Compilation

Each C source file is compiled with the freestanding cross toolchain:

```bash
i686-elf-gcc -I<dirs> \
    -ffreestanding -m32 -fno-pie -fno-stack-protector \
    -nostdlib -nostartfiles -MMD -MP -MF <depfile> \
    -c file.c -o build/obj/<subdir>/file.o
```

Each assembly file is assembled to ELF object form:

```bash
nasm -f elf32 file.asm -o build/obj/<subdir>/file.o
```

C depfiles (`.d`) are emitted alongside object files so header changes rebuild the right targets automatically.

## Linking

All kernel objects are linked into `build/bin/kernel.elf`:

```bash
i686-elf-ld -T linker.ld -m elf_i386 <objects> -o build/bin/kernel.elf
```

## Linker Script

`linker.ld` sets the kernel load address and exports BSS boundary symbols:

```ld
ENTRY(_start)

SECTIONS
{
    . = 0x1000;

    .text   : { *(.text)   }
    .rodata : { *(.rodata) }
    .data   : { *(.data)   }

    .bss :
    {
        bss_start = .;
        *(.bss)
        *(COMMON)
        bss_end = .;
    }
}
```

`bss_start` and `bss_end` are used by `kernel_entry.asm` to zero the BSS region at boot. The PMM bitmap lives in BSS and must be zero before `pmm_init()` — the BSS zeroing step handles this automatically.

## Binary conversion

```bash
i686-elf-objcopy -O binary kernel.elf kernel.bin
```

This strips all ELF metadata. The result is a flat binary. The `.bss` section has no representation in this file — it is zero-initialized at runtime by `kernel_entry.asm`.

---

# Layout Constant Ownership

SmallOS keeps disk-layout constants in the source files or tools that own them. The Makefile discovers those declarations and passes them into the image-building steps rather than redefining the numbers itself.

Current ownership:

```text
src/boot/boot.asm
  BOOT_SECTOR_SIZE
  MBR_PARTITION_TABLE_OFFSET
  MBR_PARTITION_ENTRY_SIZE

src/boot/loader2.asm
  LOADER2_SIZE_BYTES

tools/mkfat16.c
  TOTAL_SIZE_MB
  TOTAL_SECTORS
```

This keeps the boot-stage source files and filesystem tool as the single source of truth for image-layout facts.

---

# User Programs (ELF)

User programs are compiled separately and packed into the FAT16 image. Adding or changing a program rebuilds the FAT16 image and final disk image, but does not require relinking `kernel.elf` or regenerating `kernel.bin` unless kernel sources also change.

## Source files

```text
src/user/hello.c
src/user/ticks.c
src/user/args.c
src/user/runelf_test.c
src/user/readline.c
src/user/exec_test.c
src/user/fileread.c
src/user/compiler_demo.c
src/user/heapprobe.c
src/user/statprobe.c
src/user/fileprobe.c
src/user/cwdprobe.c
src/user/sleep_test.c
src/user/ptrguard.c
src/user/preempt_test.c
src/user/fault.c
src/user/user_alloc.c
src/user/user_posix.c
```

All use `user_lib.h` and `user_syscall.h`. No libc, no hosted runtime, and no dynamic linking.

The guest compiler toolchain ships as `tools/tcc.elf`, built from the vendored
TinyCC sources with a SmallOS-target wrapper. The guest entry point is a
SmallOS-side driver that calls libtcc directly instead of relying on TinyCC's
hosted CLI `main()`, which keeps the compiler path stable in the freestanding
guest runtime. The shell selftests compile `samples/tccmath.c`,
`samples/tccagg.c`, `samples/tcctree.c`, and `samples/tccmini.c` inside the
guest with that compiler.

## Compile

```bash
i686-elf-gcc -I<dirs> \
    -ffreestanding -m32 -fno-pie -fno-stack-protector \
    -nostdlib -nostartfiles -MMD -MP -MF build/obj/user/hello.d \
    -c hello.c -o build/obj/user/hello.o
```

## Link

All user programs are linked at `USER_CODE_BASE` (0x400000):

```bash
i686-elf-ld -m elf_i386 -Ttext-segment 0x400000 -e _start \
    build/obj/user/hello.o -o build/bin/hello.elf
```

Key link options:

* `-Ttext-segment 0x400000` — virtual load address, must match `USER_CODE_BASE` in `paging.h`
* `-e _start` — entry point symbol
* no `-T linker.ld` — user programs use a simpler layout than the kernel

Multiple programs sharing `-Ttext-segment 0x400000` is safe because each `runelf` invocation creates its own page directory, mapping that virtual address to different physical frames.

## Properties

* fixed virtual address `0x400000` — must match where the ELF loader maps segments
* default entry point `_start(int argc, char** argv)`
* hosted-style tools may link `src/user/user_crt0.c` and provide `main(int argc, char** argv)`
* SmallOS runtime only, no external libc
* output via syscalls only (`sys_write`, `sys_putc`)
* direct `_start` programs must call `sys_exit(status)`; `user_crt0` does that for `main`

---

# FAT16 Image

## Tool

`tools/mkfat16.c` is a host C program compiled by the Makefile:

```makefile
$(TOOLS_DIR)/mkfat16: tools/mkfat16.c | dirs
    $(HOST_CC) -o $@ $<
```

## Building

```bash
build/tools/mkfat16 build/bin/fat16.img \
    build/bin/hello.elf \
    build/bin/ticks.elf \
    build/bin/args.elf \
    build/bin/runelf_test.elf \
    build/bin/readline.elf \
    build/bin/exec_test.elf \
    build/bin/fileread.elf \
    build/bin/compiler_demo.elf \
    build/bin/heapprobe.elf \
    build/bin/statprobe.elf \
    build/bin/fileprobe.elf \
    build/bin/sleep_test.elf \
    build/bin/ptrguard.elf \
    build/bin/preempt_test.elf \
    build/bin/fault.elf
```

`mkfat16` produces a raw FAT16 volume containing the shipped utilities in the
root directory plus the `apps/demo/`, `apps/tests/`, and `tools/` subtrees.

Shipped FAT16 programs:
- `apps/demo/hello` - print argc/argv and tick count
- `apps/tests/ticks` - print the current tick count
- `apps/tests/args` - print argc and argv
- `apps/tests/runelf_test` - verify ELF loading, syscalls, and stack setup
- `apps/tests/readline` - interactive SYS_READ demo
- `apps/tests/exec_test` - exercise SYS_EXEC semantics
- `apps/tests/fileread` - exercise VFS-backed file handles via SYS_OPEN / SYS_FREAD / SYS_CLOSE
- `apps/tests/compiler_demo` - exercise SYS_WRITEFILE, SYS_WRITEFILE_PATH, and readback
- `apps/tests/heapprobe` - exercise malloc/free/realloc/calloc
- `apps/tests/statprobe` - exercise SYS_STAT and path probing
- `apps/tests/fileprobe` - exercise file wrapper helpers, rename, unlink, and stat
- `apps/tests/cwdprobe` - exercise process cwd and relative path syscalls
- `apps/tests/sleep_test` - exercise SYS_SLEEP semantics
- `apps/tests/ptrguard` - exercise syscall pointer validation
- `apps/tests/preempt_test` - prove timer-driven preemption
- `apps/tests/fault` - fault probe (ud/gp/de/br/pf)
- `tools/tcc.elf` - SmallOS-hosted TinyCC compiler binary linked through
  `src/user/user_crt0.c`
- `samples/tccmath.c`, `samples/tccagg.c`, `samples/tcctree.c`, `samples/tccmini.c` - guest compiler test inputs used by the shell selftests

## Properties

* fixed-size volume defined by `tools/mkfat16.c`
* root directory contains the small launcher utilities and shared compiler demo artifacts
* `apps/demo/` contains the hello demo ELF
* `apps/tests/` contains the remaining shipped test ELFs
* `tools/` contains the guest TinyCC binary
* sample C sources live at the image root until the shell demo moves them into `samples/`
* filenames are converted to uppercase 8.3 names
* no external filesystem tools are required

---

# Loader2 Generation

`loader2.asm` is generated at build time so the stage-2 stack-top values can be filled in without hardcoding them:

```asm
STAGE2_STACK_TOP    equ __STAGE2_STACK_TOP__
STAGE2_STACK_TOP_32 equ __STAGE2_STACK_TOP_32__
```

The Makefile injects those values from the generated stage-2 stack contract and writes `build/gen/loader2.gen.asm` via `sed`:

```makefile
sed -e "s/__STAGE2_STACK_TOP__/0xFF00/" \
    -e "s/__STAGE2_STACK_TOP_32__/0x1FF000/" \
    loader2.asm > loader2.gen.asm
```

NASM then assembles the generated file:

```bash
nasm -f bin loader2.gen.asm -o loader2.bin
```

The size constraint is enforced by `LOADER2_SIZE_BYTES` in `loader2.asm`. The Makefile verifies the final binary size after assembly.

---

# Kernel Padding — Critical

`kernel.bin` must occupy a whole number of sectors in the final disk image before `fat16.img` is appended.

That padding is now performed by `mkimage`, not by a separate Makefile-generated `kernel_padded.bin` artifact.

**Why this is required:** The final FAT16 start LBA is computed as:

```text
kernel_lba     = 1 + loader2_sectors
FAT16_LBA      = kernel_lba + kernel_sectors
```

where:

```text
loader2_sectors = loader2.bin / BOOT_SECTOR_SIZE
kernel_sectors  = ceil(kernel.bin / BOOT_SECTOR_SIZE)
```

If `kernel.bin` is not padded to a sector boundary before `fat16.img` is appended, the FAT volume would begin mid-sector in the final image while the kernel would still try to read it from the next full LBA. FAT16 reads would then return incorrect data.

---

# Final Disk Image Construction

## Tool

`tools/mkimage.c` is a host C program compiled by the Makefile:

```makefile
$(TOOLS_DIR)/mkimage: tools/mkimage.c | dirs
    $(HOST_CC) -o $@ $<
```

## Building

The final image builder is invoked with already-built component binaries and source-owned layout constants:

```bash
build/tools/mkimage \
    --boot build/bin/boot.bin \
    --loader build/bin/loader2.bin \
    --kernel build/bin/kernel.bin \
    --fat16 build/bin/fat16.img \
    --out build/img/os-image.bin \
    --sector-size 512 \
    --loader-size 2048 \
    --boot-loader2-sectors-patch-offset 488 \
    --boot-fat16-lba-patch-offset 504
```

## What `mkimage` does

`mkimage` assembles the final disk image as:

```text
boot.bin
loader2.bin
kernel.bin
zero padding to next sector boundary
fat16.img
```

It computes:

```text
loader2_sectors = loader2.bin / BOOT_SECTOR_SIZE
kernel_lba      = 1 + loader2_sectors
kernel_sectors  = ceil(kernel.bin / BOOT_SECTOR_SIZE)
FAT16_LBA       = kernel_lba + kernel_sectors
```

It then writes the kernel and FAT16 spans into the MBR partition table entries declared by `MBR_PARTITION_TABLE_OFFSET` and `MBR_PARTITION_ENTRY_SIZE` in `boot.asm`.

## Layout

```text
LBA 0                     boot.bin
LBA 1 ... 4               loader2.bin
LBA 5 ... N               padded kernel region
LBA N+1 ...               fat16.img
```

The exact kernel span depends on `kernel.bin` size rounded up to a whole number of sectors.

---

# Stage 1 Bootloader

```text
boot.asm → build/bin/boot.bin
```

Constraints:

* boot sector size is declared by `BOOT_SECTOR_SIZE` in `boot.asm`
* the BIOS boot signature `0xAA55` must be present at the end of the sector
* the boot sector also contains a declared patch field for the FAT16 start LBA

Stage 1 uses the old CHS interface (`INT 0x13 AH=0x02`) because it only reads the fixed-size stage-2 loader, which fits comfortably within track 0.

---

# Running

```bash
make clean && make
qemu-system-i386 -drive format=raw,file=build/img/os-image.bin
```

**Do not use `-fda`** (floppy disk mode). Floppy does not support INT 0x13 LBA extended reads. The system will halt with `NO LBA!` if launched as a floppy.

---

# Common Build Failures

## fat16: bad boot signature (runtime)

Cause: FAT16 start LBA was computed or patched incorrectly, FAT16 image missing from the disk image, or launching in floppy mode. The kernel reads the wrong LBA or invalid data instead of a FAT16 boot sector.

Fix: the `mkimage` step handles kernel padding and FAT16 partition-table writing automatically. If the image is assembled manually, preserve the same layout and table rules.

## loader2.bin size error

```text
nasm: error: binary too large for `times' command
```

or

```text
ERROR: loader2.bin must be 2048 bytes, got N
```

Cause: code + strings in loader2 exceed `LOADER2_SIZE_BYTES`. Remove or shorten debug message strings.

## Undefined `bss_start` / `bss_end` (NASM error)

```text
src/boot/kernel_entry.asm: error: symbol 'bss_start' not defined
```

Cause: symbols not declared `extern` in `kernel_entry.asm`, or not exported in `linker.ld`.

Fix: ensure `extern bss_start` / `extern bss_end` in `kernel_entry.asm`, and `bss_start = .;` / `bss_end = .;` in the `.bss` section of `linker.ld`.

## Triple fault on boot

Cause: kernel `.bss` not zeroed. Page tables and PMM bitmap contain garbage. Verify `kernel_entry.asm` has the `rep stosb` loop and correct `extern` declarations.

## Disk read error (runtime, loader screen)

`INT 0x13 AH=0x42` returned carry set. Causes:

* launched as floppy (`-fda`) instead of hard disk — use `-drive format=raw`
* disk image too small — `fat16.img` not appended to image
* LBA out of range for the disk image size

## NO LBA! (runtime, loader screen)

BIOS does not support INT 0x13 extensions. Should not occur with QEMU IDE. Ensure `-drive format=raw` not `-fda`.

## RWX segment linker warning

```text
warning: LOAD segment with RWX permissions
```

Cause: simple linker script does not separate read-only and executable sections. No functional impact.

---

# Dependency Model

```text
apps/demo/hello.elf ────────────────────┐
apps/tests/*.elf ───────────────────────┤
tools/tcc.elf / samples/*.c ────────────┤→ fat16.img ───────┐
                                        │                           │
kernel.bin ───────────────┐             │                           │
                          ├→ loader2.gen.asm → loader2.bin ───────┤
                          │                                        │
                          └────────────────────────────────────────┤
boot.bin ──────────────────────────────────────────────────────────┤
                                                                   │
mkimage ────────────────────────────────────────────────────────────┤
                                                                   ↓
                                                             os-image.bin
```

---

# Clean Build

```bash
make clean
```

Removes the entire `build/` directory. Always use `make clean && make` when making structural changes (new source files, linker script changes, Makefile changes, host tool changes).

---

# Design Decisions

## Flat Binary Kernel

Pros: simple loader (no ELF parser in loader2), predictable layout, no runtime dependency.
Cons: no relocation, no metadata, BSS must be zeroed manually.

## All User Programs at 0x400000

All user ELFs are linked at the same virtual address. This is safe because each `runelf` creates a new page directory with its own private mapping at PD index 1. The same virtual address maps to different physical frames for different processes.

Pros: simple linking, no need for unique link addresses per program.
Cons: no PIE, no dynamic linking, and all programs must currently fit the fixed loader / exec model even though a scheduler now exists.

## FAT16 Image Instead of Embedded Programs

Current approach: ELF binaries are stored in a separate `fat16.img` partition. Kernel rebuild is not required to add or update programs. Kernel binary size is unaffected by program count.

## Generated Loader2

The Makefile generates `loader2.gen.asm` by text substitution into `loader2.asm`. This allows build-time injection of the generated stack-top values without hardcoding them.

## Host Tool Image Assembly

`mkimage` owns final disk-image assembly. This keeps Make focused on dependency orchestration while moving disk-layout mechanics (padding, LBA calculation, partition-table writing) into ordinary host-side code.

`make image-layout-check` is the companion verifier for the finished `os-image.bin`. It checks that the assembled image still matches the intended sector map and partition-table layout.

## LBA Extended Reads

Replaces CHS `AH=0x02`. Removes the 18-sector-per-track limit. Required because the kernel and FAT16 partition both live beyond what simple CHS assumptions can safely handle.

---

# Future Improvements

* True blocking `SYS_READ` — yield to scheduler on empty keyboard buffer rather than busy-polling
* Per-process file-backed handles backed by the FAT16 driver (`SYS_OPEN`, `SYS_CLOSE`)
* Copy-from-user validation in syscall pointer arguments
