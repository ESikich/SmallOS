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
fat16.img        FAT16 partition      (fixed-size FAT volume, appended after the padded kernel)
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
│             hello.elf, ticks.elf, args.elf, runelf_test.elf,
│             readline.elf, exec_test.elf, fileread.elf, fault.elf,
│             fat16.img, boot.bin, loader2.bin)
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
kernel.elf          hello.elf   ticks.elf   ...
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
  FAT16_LBA_PATCH_OFFSET

src/boot/loader2.asm
  KERNEL_LBA
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
```

All use `user_lib.h` and `user_syscall.h`. No libc, no runtime, no dynamic linking.

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
* entry point `_start(int argc, char** argv)`
* no runtime, no libc
* output via syscalls only (`sys_write`, `sys_putc`)
* must call `sys_exit(0)` before returning from `_start`

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
    build/bin/fault.elf
```

`mkfat16` produces a raw FAT16 volume containing the user ELFs in the root directory.

Shipped FAT16 programs:
- `hello` - print argc/argv and tick count
- `ticks` - print the current tick count
- `args` - print argc and argv
- `runelf_test` - verify ELF loading, syscalls, and stack setup
- `readline` - interactive SYS_READ demo
- `exec_test` - exercise SYS_EXEC semantics
- `fileread` - exercise SYS_OPEN / SYS_FREAD / SYS_CLOSE
- `fault` - fault probe (ud/gp/de/br/pf)

## Properties

* fixed-size volume defined by `tools/mkfat16.c`
* root directory contains all shipped user ELFs
* filenames are converted to uppercase 8.3 names
* no external filesystem tools are required

---

# Loader2 Generation

`loader2.asm` contains one placeholder value that must be filled in at build time:

```asm
KERNEL_SECTORS      equ __KERNEL_SECTORS__
```

The Makefile computes this value from the actual size of `kernel.bin` and generates `build/gen/loader2.gen.asm` via `sed`:

```makefile
kernel_sectors=$(( ($(wc -c < kernel.bin) + (BOOT_SECTOR_SIZE - 1)) / BOOT_SECTOR_SIZE ))
sed -e "s/__KERNEL_SECTORS__/$kernel_sectors/" \
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
FAT16_LBA = KERNEL_LBA + kernel_sectors
```

where:

```text
kernel_sectors = ceil(kernel.bin / BOOT_SECTOR_SIZE)
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
    --kernel-lba 5 \
    --loader-size 2048 \
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
kernel_sectors = ceil(kernel.bin / BOOT_SECTOR_SIZE)
FAT16_LBA      = KERNEL_LBA + kernel_sectors
```

It then patches `FAT16_LBA` as a little-endian `u32` into the boot-sector field declared by `FAT16_LBA_PATCH_OFFSET` in `boot.asm`.

## Layout

```text
LBA 0                     boot.bin
LBA 1 ... KERNEL_LBA-1    loader2.bin
LBA KERNEL_LBA ... N      padded kernel region
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

Fix: the `mkimage` step handles kernel padding and FAT16 LBA patching automatically. If the image is assembled manually, preserve the same layout and patching rules.

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
hello.elf ──────────────────────────────┐
ticks.elf ──────────────────────────────┤
args.elf / runelf_test.elf / ... / fault.elf ──┤→ fat16.img ───────┐
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

The Makefile generates `loader2.gen.asm` by text substitution into `loader2.asm`. This allows build-time injection of `KERNEL_SECTORS` without hardcoding it.

## Host Tool Image Assembly

`mkimage` owns final disk-image assembly. This keeps Make focused on dependency orchestration while moving disk-layout mechanics (padding, LBA calculation, boot-sector patching) into ordinary host-side code.

## LBA Extended Reads

Replaces CHS `AH=0x02`. Removes the 18-sector-per-track limit. Required because the kernel and FAT16 partition both live beyond what simple CHS assumptions can safely handle.

---

# Future Improvements

* True blocking `SYS_READ` — yield to scheduler on empty keyboard buffer rather than busy-polling
* Per-process file descriptors backed by the FAT16 driver (`SYS_OPEN`, `SYS_CLOSE`)
* Copy-from-user validation in syscall pointer arguments
