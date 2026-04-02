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
boot.bin         stage 1 bootloader   (512 bytes, exactly)
loader2.bin      stage 2 loader       (2048 bytes, exactly)
kernel.bin       kernel               (padded to 512-byte sector boundary in image)
fat16.img        FAT16 partition      (16 MB, appended after the kernel)
```

---

# Toolchain

```text
nasm             → assembly (boot stages, interrupt stubs, kernel entry)
i686-elf-gcc     → C compilation (freestanding, 32-bit, no stdlib)
i686-elf-ld      → linking
i686-elf-objcopy → strip ELF metadata → flat binary
gcc              → host tool compilation (mkfat16)
```

---

# Build Output Structure

```text
build/
├── bin/   → final binaries (kernel.elf, kernel.bin, kernel_padded.bin,
│             hello.elf, ticks.elf, args.elf, runelf_test.elf,
│             readline.elf, exec_test.elf, fat16.img, boot.bin, loader2.bin)
├── obj/   → object files (.o)
├── gen/   → generated source (loader2.gen.asm)
├── img/   → final disk image (os-image.bin)
└── tools/ → host tools (mkfat16)
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
  ↓
kernel_padded.bin   ← padded to 512-byte sector boundary
  ↓
loader2.gen.asm     ← KERNEL_SECTORS injected
  ↓
loader2.bin
  ↓
fat16.img           ← built by build/tools/mkfat16 from all user ELFs
  ↓
os-image.bin = boot.bin + loader2.bin + kernel_padded.bin + fat16.img
```

---

# Kernel Build

## Compilation

Each C source file:

```bash
i686-elf-gcc -ffreestanding -m32 -fno-pie -fno-stack-protector \
    -nostdlib -nostartfiles -I<dirs> \
    -c file.c -o build/obj/file.o
```

Each assembly file:

```bash
nasm -f elf32 file.asm -o build/obj/file.o
```

## Linking

All objects linked into `build/bin/kernel.elf`:

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

Strips all ELF metadata. The result is a flat binary. The `.bss` section has no representation in this file — it is zero-initialized at runtime by `kernel_entry.asm`.

---

# User Programs (ELF)

User programs are compiled separately and packed into the FAT16 image. No kernel rebuild is needed to add or change programs.

## Source files

```text
src/user/hello.c
src/user/ticks.c
src/user/args.c
```

All use `user_lib.h` and `user_syscall.h`. No libc, no runtime, no dynamic linking.

## Compile

```bash
i686-elf-gcc -ffreestanding -m32 -fno-pie -fno-stack-protector \
    -nostdlib -nostartfiles -I<dirs> \
    -c hello.c -o build/obj/hello.o
```

## Link

All user programs are linked at `USER_CODE_BASE` (0x400000):

```bash
i686-elf-ld -m elf_i386 -Ttext-segment 0x400000 -e _start \
    build/obj/hello.o -o build/bin/hello.elf
```

Key link options:

* `-Ttext-segment 0x400000` — virtual load address, must match `USER_CODE_BASE` in `paging.h`
* `-e _start` — entry point symbol
* no `-T linker.ld` — user programs use a simpler layout than the kernel

Multiple programs sharing `-Ttext-segment 0x400000` is safe because each `runelf` invocation creates its own page directory, mapping that virtual address to different physical frames.

## Properties

* fixed virtual address 0x400000 — must match where the ELF loader maps segments
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
    build/bin/exec_test.elf
```

`mkfat16` produces a raw FAT16 volume containing the user ELFs in the root directory.

## Properties

* fixed 16 MB volume
* root directory contains all shipped user ELFs
* filenames are converted to uppercase 8.3 names
* no external filesystem tools are required

# Loader2 Generation

`loader2.asm` contains one placeholder value that must be filled in at build time:

```asm
KERNEL_SECTORS      equ __KERNEL_SECTORS__
```

The Makefile computes these and generates `build/gen/loader2.gen.asm` via `sed`:

```makefile
kernel_sectors=$(( ($(wc -c < kernel.bin) + 511) / 512 ))
sed -e "s/__KERNEL_SECTORS__/$kernel_sectors/" \
    loader2.asm > loader2.gen.asm
```

The FAT16 partition begins at `5 + kernel_sectors` because the kernel starts at LBA 5 and occupies `kernel_sectors` sectors. This calculation is only valid if `kernel.bin` is sector-aligned in the image — see Kernel Padding below.

NASM then assembles the generated file:

```bash
nasm -f bin loader2.gen.asm -o loader2.bin
```

The size constraint is enforced by `times 2048-($-$$) db 0` in the source. NASM errors if the code exceeds 2048 bytes.

---

# Kernel Padding — Critical

`kernel.bin` is padded to a 512-byte sector boundary before the disk image is assembled:

```bash
kernel_size=$(wc -c < build/bin/kernel.bin)
padded=$(( (kernel_size + 511) & ~511 ))
pad=$(( padded - kernel_size ))
cp build/bin/kernel.bin build/bin/kernel_padded.bin
dd if=/dev/zero bs=1 count=$pad >> build/bin/kernel_padded.bin
```

**Why this is required:** The Makefile calculates `FAT16_LBA = 5 + kernel_sectors`. `kernel_sectors` is `ceil(kernel.bin / 512)`. If `kernel.bin` is not a multiple of 512 bytes, the FAT16 partition will start mid-sector in the image while the kernel will look for it at the next full LBA, causing FAT16 reads to return incorrect data.

The padded file `kernel_padded.bin` is used only in the final `cat` — not for linking or any other step.

---

# Disk Image Construction

```bash
cat boot.bin loader2.bin kernel_padded.bin fat16.img > os-image.bin
```

## Layout

```text
LBA 0         boot.bin              (512 bytes = 1 sector)
LBA 1–4       loader2.bin           (2048 bytes = 4 sectors)
LBA 5–N       kernel_padded.bin     (ceil(kernel.bin/512) sectors)
LBA N+1+      fat16.img
```

Loader2 hardcodes the kernel LBA (5). FAT16_LBA is computed at build time and patched into the boot sector.

---

# Stage 1 Bootloader

```text
boot.asm → build/bin/boot.bin
```

Constraint: exactly 512 bytes, enforced by `times 510-($-$$) db 0` and the boot signature `dw 0xAA55`. The BIOS refuses to boot a sector without `0xAA55` at offset 510.

Stage 1 uses the old CHS interface (`INT 0x13 AH=0x02`) because it only reads 4 sectors (loader2), which fits comfortably within track 0.

---

# Running

```bash
make clean && make
qemu-system-i386 -drive format=raw,file=build/img/os-image.bin
```

**Do not use `-fda`** (floppy disk mode). Floppy does not support INT 0x13 LBA extended reads. The system will halt with "NO LBA!" if launched as a floppy.

---

# Common Build Failures

## fat16: bad boot signature (runtime)

Cause: `kernel.bin` not padded to sector boundary, FAT16 image missing from the disk image, or launching in floppy mode. The kernel reads the wrong LBA or invalid data instead of a FAT16 boot sector.

Fix: the `kernel_padded.bin` step in the Makefile image rule handles this automatically. If the image is assembled manually, ensure padding is applied.

## loader2.bin size error

```text
nasm: error: binary too large for `times' command
```

or

```text
ERROR: loader2.bin must be 2048 bytes, got N
```

Cause: code + strings in loader2 exceed 2048 bytes. Remove or shorten debug message strings.

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

* Launched as floppy (`-fda`) instead of hard disk — use `-drive format=raw`
* Disk image too small — fat16.img not appended to image
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
args.elf / runelf_test.elf / ... ──────┤→ fat16.img ───────────────────────┐
                                        │                                    │
kernel.bin ────────────────────────────┤→ kernel_padded.bin ───────────────┤
                                        │                                    │
kernel.bin ────────────────────────────┘→ loader2.gen.asm → loader2.bin ──┤
                                                                             │
boot.bin ────────────────────────────────────────────────────────────────── ┤
                                                                             ↓
                                                                       os-image.bin
```

---

# Clean Build

```bash
make clean
```

Removes the entire `build/` directory. Always use `make clean && make` when making structural changes (new source files, linker script changes, Makefile changes).

---

# Design Decisions

## Flat Binary Kernel

Pros: simple loader (no ELF parser in loader2), predictable layout, no runtime dependency.
Cons: no relocation, no metadata, BSS must be zeroed manually.

## All User Programs at 0x400000

All user ELFs are linked at the same virtual address. This is safe because each `runelf` creates a new page directory with its own private mapping at PD index 1. The same virtual address maps to different physical frames for different processes.

Pros: simple linking, no need for unique link addresses per program.
Cons: no PIE, no dynamic linking, programs cannot be run concurrently (no scheduler yet anyway).

## FAT16 Image Instead of Embedded Programs

Current approach: ELF binaries are stored in a separate `fat16.img` partition. Kernel rebuild is not required to add or update programs. Kernel binary size is unaffected by program count.

## Generated Loader2

The Makefile generates `loader2.gen.asm` by text substitution into `loader2.asm`. This allows build-time injection of `KERNEL_SECTORS` without hardcoding it.

## LBA Extended Reads

Replaces CHS `AH=0x02`. Removes the 18-sector-per-track limit. Required because the kernel and FAT16 partition both live beyond what simple CHS assumptions can safely handle.

---

# Future Improvements

* Better filesystem-backed program loading ergonomics
* Eventually make user ELFs scheduler-owned tasks