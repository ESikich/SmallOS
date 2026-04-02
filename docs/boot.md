# Boot Process Deep Dive

This document explains how SimpleOS boots from BIOS to kernel, including disk layout, memory usage, and design constraints.

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
LBA 0         → boot.bin              (512 bytes, exactly)
LBA 1–4       → loader2.bin           (2048 bytes, exactly)
LBA 5+        → kernel_padded.bin     (sector-aligned)
LBA 5+ks      → fat16.img             (16 MB FAT16 partition)
```

where `ks = ceil(kernel.bin / 512)`.

Constraints:

* `boot.bin` must be **exactly 512 bytes**
* `loader2.bin` must be **exactly 2048 bytes (4 sectors)**
* kernel begins at **LBA 5** (0-based)
* `kernel.bin` must be **padded to a 512-byte sector boundary** in the image so the FAT16 partition starts at a clean LBA
* FAT16 partition starts at `5 + kernel_sectors`
* FAT16 LBA is patched as a little-endian u32 into byte offset 504 of the boot sector after image assembly; the kernel reads it via ATA at boot

---

# Stage 1 – boot.asm

## Location

Loaded by BIOS at `0x0000:0x7C00`.

## Responsibilities

* Initialize segment registers and stack at `0x9000`
* Print debug messages via BIOS `int 0x10`
* Load stage 2 from disk to `0xA000`
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

Loaded to `0x0000:0xA000`.

## Responsibilities

* Check INT 0x13 LBA extension support — halt with message if unsupported
* Load kernel from disk (LBA 5) to physical `0x1000`
* Setup temporary GDT
* Switch to 32-bit protected mode
* Jump to kernel entry at `0x1000`

## Safe Kernel Size

Loader2 sits at `0xA000`. The kernel loads to `0x1000`. The kernel must not grow large enough to overwrite loader2 during the INT 0x13 read:

```text
safe kernel size = (0xA000 - 0x1000) / 512 = 72 sectors = 36 KB
```

Current kernel is ~58 sectors. If the kernel exceeds 72 sectors, move loader2 to `0xB000` and update `LOADER2_OFFSET` in `boot.asm` and `[org]` in `loader2.asm`.

**Symptom of violation:** BIOS INT 0x13 hangs silently mid-transfer. The screen shows `Loading...` but never advances. No error is printed because the hang occurs inside the BIOS call.

---

## Why LBA Extended Reads

Stage 2 uses **INT 0x13 AH=0x42** (LBA extended read) for all disk reads. CHS was abandoned because:

* The kernel is currently 58 sectors and the FAT16 partition starts at LBA ~63 — both exceed one CHS track (18 sectors on a standard floppy geometry).
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
LBA 5, KERNEL_SECTORS sectors
ES:BX = 0x0000:0x1000  →  physical 0x1000
```

`KERNEL_SECTORS` is injected by the Makefile at build time. It is the only placeholder in `loader2.asm`.

---

## Injected Values

The Makefile generates `build/gen/loader2.gen.asm` by replacing one placeholder in `loader2.asm`:

```text
__KERNEL_SECTORS__      ceil(kernel.bin / 512)
```

---

## Protected Mode Transition

```asm
cli
lgdt [gdt_descriptor]

mov eax, cr0
or eax, 0x1
mov cr0, eax

jmp CODE_SEG:init_pm
```

The far jump flushes the instruction pipeline and transitions the CPU to 32-bit protected mode.

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

    mov ebp, 0x90000
    mov esp, ebp

    mov eax, 0x1000
    jmp eax
```

Segment registers loaded, stack set to `0x90000`, then execution jumps to the kernel entry point.

The loader2 GDT is temporary. The kernel installs its own GDT as the very first call in `kernel_main()`. Relying on the loader2 GDT for interrupt handling results in a triple fault.

---

## Loader2 Size Constraint

Loader2 must be exactly 2048 bytes:

```asm
times 2048-($-$$) db 0
```

If the code exceeds 2048 bytes, NASM will error at assembly time.

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
0x00007C00   stage 1 (boot.asm) — done after jump to 0xA000
0x0000A000   stage 2 (loader2.asm) — done after jump to 0x1000
0x00001000   kernel entry point (_start in kernel_entry.asm)
0x00090000   stack top (set up by loader2's init_pm)
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

`kernel.bin` is padded to a 512-byte sector boundary before the disk image is assembled:

```bash
kernel_size=$(wc -c < kernel.bin)
padded=$(( (kernel_size + 511) & ~511 ))
pad=$(( padded - kernel_size ))
dd if=/dev/zero bs=1 count=$pad >> kernel_padded.bin
cat boot.bin loader2.bin kernel_padded.bin fat16.img > os-image.bin
```

Without this padding, the FAT16 partition does not start on a clean 512-byte boundary. The Makefile calculates `FAT16_LBA = 5 + kernel_sectors`, which assumes the FAT16 image begins exactly `kernel_sectors` sectors after LBA 5. An unpadded kernel causes FAT16 reads to land mid-sector and return all zeros.

---

# FAT16 LBA Delivery

The FAT16 partition start LBA cannot be a compile-time constant in `fat16.c` without a chicken-and-egg dependency (kernel compilation needs it, but it depends on kernel size). Instead:

1. The Makefile computes `fat16_lba = 5 + kernel_sectors` after building `kernel.bin`
2. It patches the value as a little-endian u32 into byte offset 504 of `boot.bin` in the assembled image using `dd conv=notrunc`
3. At runtime, `fat16_init()` reads ATA sector 0 and extracts the value from offset 504

Byte offset 504 of the boot sector is in the zero-padded area between the bootloader code and the `0x55AA` signature at bytes 510–511. It is safe to overwrite.

---

# Bootloader Contract

The following must always hold:

* kernel starts at LBA 5 in the disk image
* kernel is loaded to physical `0x1000`
* `kernel.bin` is padded to a 512-byte boundary in the image
* FAT16 partition starts at `5 + kernel_sectors` (LBA)
* FAT16 LBA is patched into boot sector offset 504 after image assembly
* loader2 GDT is temporary — kernel installs its own GDT first
* segment registers correctly initialized before protected mode entry
* `bss_start` / `bss_end` correctly defined and BSS zeroed before `paging_init()`
* loader2 address + 2048 ≥ kernel load end (currently: `0xA000 > 0x1000 + 58*512 = 0x8440` ✓)

Violation of any of these results in an immediate crash or silent corruption.

---

# Failure Modes

## Hangs at "Loading..." with no error

The kernel load overwrote loader2 mid-transfer. The kernel has grown beyond `(loader2_address - 0x1000) / 512` sectors. Move loader2 to a higher address.

## "Disk read error!" on screen

`INT 0x13 AH=0x42` returned carry set. Causes:

* Drive not presented as hard disk — use `-drive format=raw,file=...` not `-fda`
* LBA address out of range — disk image too small or fat16.img not appended

## "NO LBA!" on screen

BIOS does not support INT 0x13 extensions. Ensure QEMU is not in floppy mode.

## "fat16: bad boot signature" or "fat16: LBA not patched"

The sector-0 patch failed or the FAT16 image was not appended. Check the `dd conv=notrunc` step in the Makefile `os-image.bin` rule.

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
Stage 1  →  load stage 2 (CHS, 4 sectors, to 0xA000)
Stage 2  →  LBA extension check
         →  load kernel (LBA 5 → 0x1000, KERNEL_SECTORS sectors)
         →  protected mode entry
Kernel   →  zero BSS
         →  gdt_init, paging_init, memory_init, pmm_init
         →  keyboard, timer, idt, sched_init
         →  ata_init, fat16_init
         →  create shell task, sti, sched_start
```

Boot code is the most fragile part of the system. Failures here are often silent — no terminal, no debug output, just a reboot loop or a hung screen.

---

# Future Work

* Replace fixed disk layout with a partition table or boot protocol
* Support kernels larger than 72 sectors (move loader2 to `0xB000`)
* Multiboot-style boot protocol for GRUB compatibility