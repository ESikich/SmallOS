# Boot Process Deep Dive

This document explains how SmallOS boots from BIOS to kernel, including disk layout, memory usage, and design constraints.

---

# Overview

```text
BIOS
 → boot.asm        (stage 1, 512 bytes, CHS read of loader2)
 → loader2.asm     (stage 2, 2048 bytes, LBA read of kernel only)
 → kernel.bin      (loaded to 0x1000)
 → protected mode
 → kernel_entry.asm  (zeros BSS, calls kernel_main)
 → kernel_main()
```

---

# Disk Layout

The OS image (`os-image.bin`) is structured as:

```text
LBA 0           → boot.bin              (512 bytes, exactly)
LBA 1–4         → loader2.bin           (currently 2048 bytes, exactly)
LBA KERNEL_LBA+ → padded kernel region  (sector-aligned)
LBA KERNEL_LBA+ks
               → fat16.img             (16 MB FAT16 partition)
```

where `ks = ceil(kernel.bin / BOOT_SECTOR_SIZE)`.

Constraints:

* `boot.bin` must be **exactly `BOOT_SECTOR_SIZE` bytes**
* `loader2.bin` must be **exactly `LOADER2_SIZE_BYTES` bytes** (currently 2048 bytes / 4 sectors)
* kernel begins at **`KERNEL_LBA`** (currently 5, 0-based)
* `kernel.bin` must be **padded to a sector boundary during final image assembly** so the FAT16 partition starts at a clean LBA
* FAT16 partition starts at `KERNEL_LBA + kernel_sectors`
* FAT16 LBA is patched as a little-endian u32 into the field declared by `FAT16_LBA_PATCH_OFFSET` in `boot.asm` (currently byte offset 504 of sector 0); the kernel reads it during `fat16_init()`

---

# Stage 1 – boot.asm

## Location

Loaded by BIOS at `0x0000:0x7C00`.

## Responsibilities

* Initialize segment registers and a temporary stage-1 stack at `0x9000`
* Print debug messages via BIOS `int 0x10`
* Load stage 2 from disk to `0x10000`
* Transfer control to stage 2

## BIOS Disk Read

Stage 1 uses the CHS interface for loader2 only. Loader2 is 4 sectors and fits within the first track.

```asm
int 0x13
AH = 0x02  (read sectors)
AL = number of sectors
CH = cylinder
CL = sector (1-based)
DH = head
DL = drive
```

## Why Stage 1 Is Minimal

Must fit in 512 bytes. No room for LBA logic, extension checks, or complex error handling. Stage 1 performs only essential loading and defers all complexity to stage 2.

---

# Stage 2 – loader2.asm

## Location

Loaded to `0x1000:0x0000` (physical `0x10000`).

## Responsibilities

* Check INT 0x13 LBA extension support — halt with message if unsupported
* Load kernel from disk (`KERNEL_LBA`, currently 5) to physical `0x1000`
* Setup temporary GDT
* Switch to 32-bit protected mode
* Jump to kernel entry at `0x1000`

## Safe Kernel Size

Loader2 now sits at `0x10000`. The kernel loads to `0x1000`. Stage 2 now uses a loader-segment stack, so the real-mode `SP` is `0xFF00` while the physical stack top is well above the loader body. The kernel must not grow large enough to overwrite the loader during the INT 0x13 read:

```text
safe kernel size = (0x10000 - 0x1000) / 512 = 120 sectors = 60 KiB
```

If the kernel exceeds 120 sectors, the build fails before image assembly. The ceiling is computed from the loader2 load address and the generated stage-2 stack top, so any future layout bump must update both in `Makefile` and `loader2.asm`.

## Layout Ownership

The boot stages own the disk-layout constants they depend on:

* `boot.asm` declares `BOOT_SECTOR_SIZE`
* `boot.asm` declares `FAT16_LBA_PATCH_OFFSET`
* `loader2.asm` declares `KERNEL_LBA`
* `loader2.asm` declares `LOADER2_SIZE_BYTES`

The Makefile reads these declarations during image construction rather than redefining the numbers independently, injects the generated kernel-sector and stack-top values into `loader2.asm`, and passes the resulting layout facts into `mkimage` for final disk-image assembly.

## Layout Check

`make boot-layout-check` verifies the built boot artifacts and the generated loader template before the final image is assembled. It checks:

* `boot.bin` is exactly 512 bytes
* `loader2.bin` is exactly 2048 bytes
* loader2 still loads at `0x10000`
* the generated stage-2 stack and the kernel boot stack match the current contract
* the kernel still fits below both the loader body and the stage-2 stack

This keeps the hand-rolled boot path explicit: the build fails before QEMU starts if the layout drifts.

`make image-layout-check` goes one step further and validates the finished `os-image.bin` itself. It checks the boot-sector patch, the loader and kernel placement, the FAT16 start LBA, and the zero padding between the kernel and FAT16 region.

**Symptom of violation:** BIOS INT 0x13 hangs silently mid-transfer. The screen shows `Loading...` but never advances. No error is printed because the hang occurs inside the BIOS call.

---

## Why LBA Extended Reads

Stage 2 uses **INT 0x13 AH=0x42** (LBA extended read) for all disk reads. CHS was abandoned because:

* The kernel and FAT16 partition can easily extend beyond one CHS track (18 sectors on a standard floppy geometry).
* CHS reads beyond sector 18 on track 0 either fail or silently read wrong data.
* LBA addressing has no track geometry limit.

**The system must be launched as a hard disk** (`-drive format=raw,file=...` in QEMU). Floppy mode (`-fda`) does not support INT 0x13 extensions and will halt with "NO LBA!".

---

## LBA Extension Check

Before any disk read, stage 2 verifies the BIOS supports INT 0x13 extensions:

```asm
mov ah, 0x41
mov bx, 0x55AA
mov dl, [BOOT_DRIVE]
int 0x13
jc .no_ext          ; carry set = not supported
```

If not supported, loader2 prints "NO LBA!" and halts.

---

## Disk Address Packet (DAP)

All LBA reads use a DAP structure passed via `SI`:

```asm
dap:
    db 0x10       ; packet size = 16 bytes
    db 0x00       ; reserved
    dw count      ; number of sectors to read
    dw offset     ; destination buffer offset
    dw segment    ; destination buffer segment
    dd lba_low    ; LBA bits 0–31
    dd lba_high   ; LBA bits 32–63 (always 0 for us)
```

Called via:

```asm
mov ah, 0x42
mov dl, [BOOT_DRIVE]
mov si, dap
int 0x13
jc disk_error
```

---

## Kernel Loading

```text
LBA KERNEL_LBA, KERNEL_SECTORS sectors
ES:BX = 0x0000:0x1000  →  physical 0x1000
```

`KERNEL_SECTORS` is injected by the Makefile at build time. It is the only placeholder in `loader2.asm`.

---

## Injected Values

The Makefile generates `build/gen/loader2.gen.asm` by replacing one placeholder in `loader2.asm`:

```text
__KERNEL_SECTORS__      ceil(kernel.bin / BOOT_SECTOR_SIZE)
```

---

## Protected Mode Transition

```asm
cli
lgdt [gdt_descriptor]

mov eax, cr0
or eax, 0x1
mov cr0, eax

jmp dword CODE_SEG:init_pm
```

The far jump flushes the instruction pipeline and transitions the CPU to 32-bit protected mode. Because loader2 lives above 64 KiB, the jump uses a 32-bit offset so the `init_pm` entry address is not truncated.

---

## 32-bit Initialization

```asm
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    mov ebp, 0x1FF000
    mov esp, ebp

    mov eax, 0x1000
    jmp eax
```

Segment registers loaded, stack set to `0x1FF000`, then execution jumps to the kernel entry point.

The loader2 GDT is temporary. Early in `kernel_main()`, the kernel installs its own GDT with `gdt_init()` before interrupts are enabled. Relying on the loader2 GDT for interrupt handling results in a triple fault.

---

## Loader2 Size Constraint

Loader2 must be exactly `LOADER2_SIZE_BYTES` bytes. The source enforces a fixed-size binary layout, and the Makefile verifies the final assembled size.

---

# Kernel Entry – kernel_entry.asm

```asm
[bits 32]
global _start
extern kernel_main
extern bss_start
extern bss_end

_start:
    mov edi, bss_start
    mov ecx, bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb            ; zero BSS — page tables live here

    call kernel_main

hang:
    cli
    hlt
    jmp hang
```

BSS zeroing is the first thing that happens in protected mode. The kernel's three paging structures (`kernel_page_directory`, `low_page_table_0`, `low_page_table_1`) are static arrays in `.bss`. Without zeroing they contain garbage and `paging_init()` immediately triple-faults.

`bss_start` and `bss_end` are linker symbols from `linker.ld`. Both must be declared `extern` in the asm file.

---

# Memory Map During Boot

```text
0x00007C00   stage 1 (boot.asm) — done after jump to 0x10000
0x00010000   stage 2 (loader2.asm) — done after jump to 0x1000
0x00001000   kernel entry point (_start in kernel_entry.asm)
0x001FF000   stack top (set up by loader2's init_pm)
```

---

# Why Two Stages Exist

Stage 1 cannot fit LBA logic, protected mode setup, or disk reads in 512 bytes. Stage 2 carries all of that complexity.

| Stage   | Purpose                                          |
| ------- | ------------------------------------------------ |
| Stage 1 | load stage 2 via CHS (4 sectors, simple)         |
| Stage 2 | LBA extension check, kernel load, protected mode |

---

# Kernel Padding — Critical Design Constraint

`kernel.bin` is padded to a `BOOT_SECTOR_SIZE` boundary during final image assembly by `mkimage`.

Without this padding, the FAT16 partition does not start on a clean sector boundary. Final image assembly computes `FAT16_LBA = KERNEL_LBA + kernel_sectors`, where `kernel_sectors = ceil(kernel.bin / BOOT_SECTOR_SIZE)`. If `kernel.bin` is not padded before `fat16.img` is appended, FAT16 reads land mid-sector and return incorrect data.

---

# FAT16 LBA Delivery

The FAT16 partition start LBA cannot be a compile-time constant in `fat16.c` without a chicken-and-egg dependency (kernel compilation needs it, but it depends on kernel size). Instead:

1. The Makefile discovers source-owned layout constants from `boot.asm` and `loader2.asm`
2. `mkimage` computes `fat16_lba = KERNEL_LBA + kernel_sectors` during final image assembly
3. `mkimage` patches the value as a little-endian u32 into the field declared by `FAT16_LBA_PATCH_OFFSET` in `boot.asm`
4. At runtime, `fat16_init()` reads ATA sector 0 and extracts the patched value

The patch field lives in the declared boot-sector padding area before the `0x55AA` signature and is safe to overwrite.

---

# Bootloader Contract

The following must always hold:

* kernel starts at `KERNEL_LBA` in the disk image
* kernel is loaded to physical `0x1000`
* `kernel.bin` is padded to a whole-sector boundary during final image assembly
* FAT16 partition starts at `KERNEL_LBA + kernel_sectors` (LBA)
* FAT16 LBA is patched into the boot-sector field declared by `FAT16_LBA_PATCH_OFFSET`
* loader2 GDT is temporary — kernel installs its own GDT first
* segment registers correctly initialized before protected mode entry
* `bss_start` / `bss_end` correctly defined and BSS zeroed before `paging_init()`
* `0x1000 + kernel_sectors * 512` must stay below both the loader2 load address and the generated stage-2 stack top

Violation of any of these results in an immediate crash or silent corruption.

---

# Failure Modes

## Hangs at "Loading..." with no error

The kernel load overwrote loader2 or the generated stage-2 stack mid-transfer. The kernel has grown beyond the generated stage-2 ceiling. Move stage 2 higher and update the matching constants in `Makefile` and `loader2.asm`.

## "Disk read error!" on screen

`INT 0x13 AH=0x42` returned carry set. Causes:

* Drive not presented as hard disk — use `-drive format=raw,file=...` not `-fda`
* LBA address out of range — disk image too small or fat16.img not appended

## "NO LBA!" on screen

BIOS does not support INT 0x13 extensions. Ensure QEMU is not in floppy mode.

## "fat16: bad boot signature" or "fat16: LBA not patched"

The sector-0 patch failed or the FAT16 image was not appended. Check the `mkimage` step in the final image build.

## Triple fault immediately after kernel loads

BSS not zeroed before `paging_init()`. Confirm:

* `kernel_entry.asm` contains the `rep stosb` BSS zeroing loop
* `extern bss_start` / `extern bss_end` declared in `kernel_entry.asm`
* `bss_start = .;` / `bss_end = .;` present in `.bss` section of `linker.ld`

## Reboot loop

Typical causes: invalid kernel GDT (must call `gdt_init()` before `sti`), invalid IDT, broken interrupt handler.

## Loader2 assembly error: binary too large

NASM rejects `times 2048-($-$$) db 0` because the code exceeds 2048 bytes. Shorten message strings or remove debug code.

---

# Debugging Techniques

## BIOS print (real mode)

```asm
mov ah, 0x0E
mov al, <char>
int 0x10
```

## Execution halt

```asm
jmp $
```

## VGA direct write (protected mode, before terminal)

```asm
mov byte [0xB8000], 'X'
mov byte [0xB8001], 0x4F   ; red background, white text
```

## QEMU logging

```bash
qemu-system-i386 -drive format=raw,file=build/img/os-image.bin \
    -no-reboot -no-shutdown \
    -d int,cpu_reset,guest_errors \
    -D qemu.log
```

---

# Design Decisions

## Fixed Disk Layout

Pros: simple, predictable, no partition table required.
Cons: inflexible, requires careful LBA calculation when sizes change.

## Fixed Load Address (0x1000)

Pros: easy to reason about, matches linker script directly.
Cons: no relocation support.

## FAT16 LBA via Boot Sector Patch

Pros: no compile-time dependency, no chicken-and-egg; the correct value is always in the image regardless of kernel size changes.
Cons: slightly unusual; the patch must not corrupt the boot signature at offset 510–511.

## No Ramdisk

Programs are loaded from the FAT16 partition at runtime via ATA PIO. The ramdisk was a temporary program store used while the FAT16 driver was being developed; it has been removed.

---

# Summary

```text
Stage 1  →  load stage 2 (CHS, fixed-size loader, to 0x10000)
Stage 2  →  LBA extension check
         →  load kernel (LBA KERNEL_LBA → 0x1000, KERNEL_SECTORS sectors)
         →  protected mode entry
Kernel   →  zero BSS
         →  terminal_init, gdt_init, paging_init, memory_init, pmm_init
         →  keyboard, timer, idt, sched_init
         →  ata_init, fat16_init
         →  create shell task, sti, sched_start
```

Boot code is the most fragile part of the system. Failures here are often silent — no terminal, no debug output, just a reboot loop or a hung screen.

---

# Future Work

* Replace fixed disk layout with a partition table or boot protocol
* Support kernels larger than 120 sectors
* Multiboot-style boot protocol for GRUB compatibility
