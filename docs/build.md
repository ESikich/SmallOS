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
kernel.bin       kernel               (padded to 512-byte sector boundary)
ramdisk.rd       ELF program archive  (built by tools/mkramdisk)
```

---

# Toolchain

```text
nasm             → assembly (boot stages, interrupt stubs, kernel entry)
i686-elf-gcc     → C compilation (freestanding, 32-bit, no stdlib)
i686-elf-ld      → linking
i686-elf-objcopy → strip ELF metadata → flat binary
gcc              → host tool compilation (mkramdisk)
```

---

# Build Output Structure

```text
build/
├── bin/   → final binaries (kernel.elf, kernel.bin, kernel_padded.bin,
│             hello.elf, ticks.elf, ramdisk.rd, boot.bin, loader2.bin)
├── obj/   → object files (.o)
├── gen/   → generated source (loader2.gen.asm)
├── img/   → final disk image (os-image.bin)
└── tools/ → host tools (mkramdisk)
```

---

# High-Level Build Flow

```text
source files (.c, .asm)
  ↓
object files (.o)
  ↓
kernel.elf          hello.elf   ticks.elf
  ↓                      ↓
kernel.bin            ramdisk.rd  ← packed by build/tools/mkramdisk
  ↓
kernel_padded.bin   ← padded to 512-byte sector boundary
  ↓
loader2.gen.asm     ← KERNEL_SECTORS / RAMDISK_SECTORS / RAMDISK_LBA injected
  ↓
loader2.bin
  ↓
os-image.bin = boot.bin + loader2.bin + kernel_padded.bin + ramdisk.rd
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

User programs are compiled separately and packed into the ramdisk. No kernel rebuild is needed to add or change programs.

## Source files

```text
src/user/hello.c
src/user/ticks.c
src/user/args.c          (not yet in ramdisk)
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
i686-elf-ld -m elf_i386 -Ttext 0x400000 -e _start \
    build/obj/hello.o -o build/bin/hello.elf
```

Key link options:

* `-Ttext 0x400000` — virtual load address, must match `USER_CODE_BASE` in `paging.h`
* `-e _start` — entry point symbol
* no `-T linker.ld` — user programs use a simpler layout than the kernel

Multiple programs sharing `-Ttext 0x400000` is safe because each `runelf` invocation creates its own page directory, mapping that virtual address to different physical frames.

## Properties

* fixed virtual address 0x400000 — must match where the ELF loader maps segments
* entry point `_start(int argc, char** argv)`
* no runtime, no libc
* output via syscalls only (`sys_write`, `sys_putc`)
* must call `sys_exit(0)` before returning from `_start`

---

# Ramdisk

## Tool

`tools/mkramdisk.c` is a host C program compiled by the Makefile:

```makefile
$(TOOLS_DIR)/mkramdisk: tools/mkramdisk.c | dirs
    $(HOST_CC) -o $@ $<
```

## Building

```bash
build/tools/mkramdisk build/bin/ramdisk.rd \
    hello:build/bin/hello.elf \
    ticks:build/bin/ticks.elf
```

`mkramdisk` produces a flat binary archive:

```text
[rd_header_t]          4-byte magic (0x52445349 'RDSI') + 4-byte file count
[rd_entry_t × count]   32-byte name + 4-byte offset + 4-byte size, per file
[file data]            raw ELF bytes, concatenated
```

All offsets are byte offsets from the start of the archive. The kernel's `ramdisk_find()` returns a pointer directly into this in-memory structure — no copies are made when looking up programs.

## Adding a program

1. Create `src/user/myprog.c` with `void _start(int argc, char** argv)` ending in `sys_exit(0)`
2. Add compile rule in Makefile
3. Add link rule at `-Ttext 0x400000`
4. Add `myprog:$(BIN_DIR)/myprog.elf` to the `ramdisk.rd` rule
5. `make clean && make`
6. `runelf myprog`

Multiple programs sharing `-Ttext 0x400000` is safe — each gets its own page directory.

---

# Loader2 Generation

`loader2.asm` contains three placeholder values that must be filled in at build time:

```asm
KERNEL_SECTORS      equ __KERNEL_SECTORS__
RAMDISK_SECTORS     equ __RAMDISK_SECTORS__
RAMDISK_LBA         equ __RAMDISK_LBA__
```

The Makefile computes these and generates `build/gen/loader2.gen.asm` via `sed`:

```makefile
kernel_sectors=$(( ($(wc -c < kernel.bin) + 511) / 512 ))
ramdisk_sectors=$(( ($(wc -c < ramdisk.rd) + 511) / 512 ))
ramdisk_lba=$(( 5 + kernel_sectors ))
sed -e "s/__KERNEL_SECTORS__/$kernel_sectors/" \
    -e "s/__RAMDISK_SECTORS__/$ramdisk_sectors/" \
    -e "s/__RAMDISK_LBA__/$ramdisk_lba/" \
    loader2.asm > loader2.gen.asm
```

`RAMDISK_LBA` = `5 + kernel_sectors` because the kernel starts at LBA 5 and occupies `kernel_sectors` sectors. This calculation is only valid if `kernel.bin` is sector-aligned in the image — see Kernel Padding below.

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

**Why this is required:** The Makefile calculates `RAMDISK_LBA = 5 + kernel_sectors`. `kernel_sectors` is `ceil(kernel.bin / 512)`. If `kernel.bin` is not a multiple of 512 bytes, the ramdisk will start mid-sector in the image but loader2 will try to read from the start of the next complete LBA — missing the beginning of the ramdisk archive including its magic bytes. The result is `ramdisk_init()` seeing zeros and printing "ramdisk: bad magic".

The padded file `kernel_padded.bin` is used only in the final `cat` — not for linking or any other step.

---

# Disk Image Construction

```bash
cat boot.bin loader2.bin kernel_padded.bin ramdisk.rd > os-image.bin
```

## Layout

```text
LBA 0         boot.bin              (512 bytes = 1 sector)
LBA 1–4       loader2.bin           (2048 bytes = 4 sectors)
LBA 5–N       kernel_padded.bin     (ceil(kernel.bin/512) sectors)
LBA N+1+      ramdisk.rd
```

Loader2 hardcodes the kernel LBA (5). The ramdisk LBA is injected at build time.

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

## ramdisk: bad magic (runtime)

Cause: `kernel.bin` not padded to sector boundary → RAMDISK_LBA points to wrong offset in image → loader2 reads zeros instead of the ramdisk archive.

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
* Disk image too small — ramdisk.rd not appended to image
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
ticks.elf ──────────────────────────────┤→ ramdisk.rd ──────────────────────┐
                                         │                                   │
kernel.bin ─────────────────────────────┤→ kernel_padded.bin ───────────────┤
                                         │                                   │
kernel.bin + ramdisk.rd ────────────────┘→ loader2.gen.asm → loader2.bin ──┤
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

## Ramdisk Instead of Embedded Programs

Old approach: ELF binaries embedded in `kernel.bin` via `incbin`. Required kernel rebuild for any program change. Increased kernel binary size.

New approach: ELF binaries packed into a separate `ramdisk.rd` archive. Kernel rebuild not required to add or update programs. Kernel binary size unaffected by program count.

## Generated Assembly for Loader2

The Makefile generates `loader2.gen.asm` by text substitution into `loader2.asm`. This allows build-time injection of `KERNEL_SECTORS`, `RAMDISK_SECTORS`, and `RAMDISK_LBA` without hardcoding them.

## LBA Extended Reads

Replaces CHS `AH=0x02`. Removes the 18-sector-per-track limit. Required because the kernel is 34+ sectors and the ramdisk starts beyond track 0.

Cons: requires hard disk mode in QEMU. Floppy mode unsupported.

---

# Future Improvements

* Filesystem-backed program loading (load ELF from FAT12 or custom FS instead of ramdisk)
* Dynamic ramdisk — reload or append without reboot
* Split debug / release builds with different optimization levels
* Multiboot protocol support (GRUB compatibility)