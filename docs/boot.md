# Boot Process Deep Dive

This document explains how SimpleOS boots from BIOS to kernel, including disk layout, memory usage, and design constraints.

---

# Overview

```text
BIOS
 → boot.asm        (stage 1, 512 bytes, CHS read of loader2)
 → loader2.asm     (stage 2, 2048 bytes, LBA reads of kernel + ramdisk)
 → kernel.bin      (loaded to 0x1000)
 → ramdisk.rd      (loaded to 0x10000)
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
LBA 5+        → kernel.bin            (padded to 512-byte boundary)
after kernel  → ramdisk.rd
```

Constraints:

* `boot.bin` must be **exactly 512 bytes**
* `loader2.bin` must be **exactly 2048 bytes (4 sectors)**
* kernel begins at **LBA 5** (0-based), the same physical location as the old "sector 6" (1-based)
* `kernel.bin` must be **padded to a 512-byte sector boundary** in the image so the ramdisk starts at a clean LBA
* `ramdisk.rd` immediately follows the padded kernel; its LBA is computed at build time as `5 + kernel_sectors`

---

# Stage 1 – boot.asm

## Location

Loaded by BIOS at:

```text
0x0000:0x7C00
```

## Responsibilities

* Initialize segment registers
* Setup stack at `0x9000`
* Print debug messages via BIOS `int 0x10`
* Load stage 2 from disk to `0x8000`
* Transfer control to stage 2

## BIOS Disk Read

Stage 1 uses the old CHS interface for loader2 only. Loader2 is only 4 sectors, so CHS works fine here.

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

Loaded to:

```text
0x0000:0x8000
```

## Responsibilities

* Check INT 0x13 LBA extension support — halt with message if unsupported
* Load kernel from disk (LBA 5) to physical `0x1000`
* Load ramdisk from disk to physical `0x10000`
* Setup temporary GDT
* Switch to 32-bit protected mode
* Jump to kernel entry at `0x1000`

---

## Why LBA Extended Reads

Stage 2 uses **INT 0x13 AH=0x42** (LBA extended read) for all disk reads. The old `AH=0x02` CHS interface was abandoned because:

* A standard floppy geometry has 18 sectors per track. The kernel is currently 34 sectors, and the ramdisk starts at LBA 39 — both exceed one track.
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

If not supported, loader2 prints "NO LBA!" and halts. This is a hard requirement — the system cannot boot without LBA support.

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

`KERNEL_SECTORS` is injected by the Makefile at build time.

---

## Ramdisk Loading

```text
LBA RAMDISK_LBA, RAMDISK_SECTORS sectors
ES:BX = 0x1000:0x0000  →  physical 0x10000
```

Both `RAMDISK_SECTORS` and `RAMDISK_LBA` are injected by the Makefile.

**Why 0x10000?** Real mode can only address memory below 1 MB (`0xFFFFF`). The ramdisk's permanent runtime address of `0x400000` is 4 MB — completely unreachable by BIOS. Loading to `0x10000` (64 KB) keeps it within the real-mode window while staying clear of all other structures. The kernel reads the ramdisk directly from `0x10000` after paging is enabled.

---

## Injected Values

The Makefile generates `build/gen/loader2.gen.asm` by replacing three placeholders in `loader2.asm`:

```text
__KERNEL_SECTORS__      ceil(kernel.bin / 512)
__RAMDISK_SECTORS__     ceil(ramdisk.rd / 512)
__RAMDISK_LBA__         5 + KERNEL_SECTORS
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

Loader2 must be exactly 2048 bytes. This is enforced in the assembly source:

```asm
times 2048-($-$$) db 0
```

If the code exceeds 2048 bytes, NASM will error at assembly time. This limits how much logic and how many message strings can fit in stage 2.

---

# Kernel Entry – kernel_entry.asm

```asm
[bits 32]
global _start
extern kernel_main
extern bss_start
extern bss_end

_start:
    ; Zero BSS — page tables live here; must be clean before paging_init()
    mov edi, bss_start
    mov ecx, bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb

    call kernel_main

hang:
    cli
    hlt
    jmp hang
```

BSS zeroing is the first thing that happens in protected mode. The kernel's three paging structures (`kernel_page_directory`, `low_page_table_0`, `low_page_table_1`) are static arrays in `.bss` at addresses `0x7000`–`0x9FFF`. Without zeroing, they contain whatever was in memory before — typically garbage — and `paging_init()` immediately triple-faults when it tries to walk the corrupt page directory.

`bss_start` and `bss_end` are linker symbols exported by `linker.ld`. Both must be declared `extern` in the asm file or NASM will error at assembly time.

The kernel is a raw flat binary — there is no ELF loader to zero `.bss` at load time. This zeroing step replaces what a normal ELF runtime would do.

---

# Memory Map During Boot

```text
0x00007C00   stage 1 (boot.asm) — done after jump to 0x8000
0x00008000   stage 2 (loader2.asm) — done after jump to 0x1000
0x00001000   kernel entry point (_start in kernel_entry.asm)
0x00010000   ramdisk (loaded by loader2, stays here permanently)
0x00090000   stack top (set up by loader2's init_pm)
```

---

# Why Two Stages Exist

Stage 1 cannot fit LBA logic, protected mode setup, or dual disk reads in 512 bytes. Stage 2 carries all of that complexity.

| Stage   | Purpose                                              |
| ------- | ---------------------------------------------------- |
| Stage 1 | load stage 2 via CHS (4 sectors, simple)             |
| Stage 2 | LBA reads, kernel + ramdisk load, protected mode     |

---

# Kernel Padding — Critical Design Constraint

`kernel.bin` is padded to a 512-byte sector boundary before the disk image is assembled:

```bash
kernel_size=$(wc -c < kernel.bin)
padded=$(( (kernel_size + 511) & ~511 ))
pad=$(( padded - kernel_size ))
dd if=/dev/zero bs=1 count=$pad >> kernel_padded.bin
cat boot.bin loader2.bin kernel_padded.bin ramdisk.rd > os-image.bin
```

Without this padding, the ramdisk does not start on a clean 512-byte boundary in the image. The Makefile calculates `RAMDISK_LBA = 5 + kernel_sectors`, which assumes the ramdisk begins exactly `kernel_sectors` sectors after LBA 5. If `kernel.bin` is e.g. 17264 bytes (not a multiple of 512), the ramdisk actually starts 368 bytes into the LBA the loader expects — loader2 reads from the wrong offset, loads zeros, and `ramdisk_init()` sees bad magic.

---

# Bootloader Contract

The following must always hold:

* kernel starts at LBA 5 in the disk image
* kernel is loaded to physical `0x1000`
* `kernel.bin` is padded to a 512-byte boundary in the image
* ramdisk starts at `5 + kernel_sectors` (LBA)
* ramdisk is loaded to physical `0x10000`
* loader2 GDT is temporary — kernel installs its own GDT first thing
* segment registers correctly initialized before protected mode entry
* `bss_start` / `bss_end` correctly defined and BSS zeroed before `paging_init()`

Violation of any of these results in an immediate crash or silent corruption.

---

# Failure Modes

## "Disk read error!" on screen

`INT 0x13 AH=0x42` returned with carry set. Causes:

* Drive not presented as hard disk — use `-drive format=raw,file=...` not `-fda`
* LBA address out of range — disk image too small or ramdisk not appended

## "NO LBA!" on screen

BIOS does not support INT 0x13 extensions. Should not occur with QEMU IDE. If seen, ensure QEMU is not in floppy mode.

## "ramdisk: bad magic" in kernel

The ramdisk bytes at `0x10000` are wrong (zeros or garbage). Root causes:

* `kernel.bin` not padded to sector boundary → RAMDISK_LBA is off → loader reads from wrong offset
* `ramdisk.rd` not appended to image (check `cat` command in Makefile image rule)
* LBA value exceeds disk image size

## Triple fault immediately after kernel loads

BSS not zeroed before `paging_init()`. Page tables contain garbage. Confirm:

* `kernel_entry.asm` contains the `rep stosb` BSS zeroing loop
* `extern bss_start` / `extern bss_end` declared in `kernel_entry.asm`
* `bss_start = .;` / `bss_end = .;` present in `.bss` section of `linker.ld`

## Reboot loop

Typical causes:

* invalid kernel GDT (must call `gdt_init()` before `sti`)
* invalid IDT
* broken interrupt handler

## Loader2 assembly error: binary too large

NASM rejects `times 2048-($-$$) db 0` because the code exceeds 2048 bytes. Shorten message strings or remove debug code.

---

# Debugging Techniques

## BIOS print (stage 1 & 2, real mode)

```asm
mov ah, 0x0E
mov al, <char>
int 0x10
```

## Execution halt

```asm
jmp $
```

Freezes execution at a known point. Useful for isolating which stage crashes.

## VGA direct write (protected mode, before terminal is up)

```asm
mov byte [0xB8000], 'X'
mov byte [0xB8001], 0x4F   ; red background, white text
```

Works immediately after `init_pm` since VGA is within the identity-mapped region.

## QEMU logging

```bash
qemu-system-i386 -drive format=raw,file=build/img/os-image.bin \
    -no-reboot -no-shutdown \
    -d int,cpu_reset,guest_errors \
    -D qemu.log
```

Repeated `INT 0x08` in the log indicates a double fault loop (triple fault). `CR3=00000000` means paging was never enabled. `CR0` bit 31 clear means paging failed or was never reached.

---

# Design Decisions

## Fixed Disk Layout

Pros: simple, predictable, no partition table required.
Cons: inflexible, requires rebuild and careful LBA calculation when sizes change.

## Fixed Load Address (0x1000)

Pros: easy to reason about, matches linker script directly.
Cons: no relocation support, no memory protection, kernel and BIOS structures share low memory.

## Ramdisk at 0x10000

Pros: below 1MB real-mode limit so BIOS can write there; identity-mapped so kernel can read it without special handling; clear of all other structures.
Cons: occupies a fixed region that cannot be reclaimed. Future per-process memory management will need to account for it.

## No Filesystem

Programs are stored in a flat archive rather than a filesystem. Pros: no filesystem driver needed, deterministic layout. Cons: programs must be packed at build time, no runtime file access.

---

# Summary

Boot process responsibilities:

```text
Stage 1  →  load stage 2 (CHS, 4 sectors)
Stage 2  →  LBA extension check
         →  load kernel (LBA 5 → 0x1000)
         →  load ramdisk (RAMDISK_LBA → 0x10000)
         →  protected mode entry
Kernel   →  zero BSS
         →  gdt_init, paging_init, memory_init
         →  drivers, interrupts
         →  ramdisk_init(0x10000)
         →  shell
```

Boot code is the most fragile part of the system. Failures here are often silent and catastrophic — no terminal, no debug output, just a reboot loop or a hung screen.

---

# Future Work

* Replace fixed disk layout with a partition table or boot protocol
* Support kernels larger than ~127 KB if needed (LBA has no such limit now)
* Filesystem-backed program loading
* Multiboot-style boot protocol for GRUB compatibility