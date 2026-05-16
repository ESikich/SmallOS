# Boot Process Deep Dive

This document explains how SmallOS boots from BIOS to kernel, including disk layout, memory usage, and design constraints.

---

# Overview

```text
BIOS
 → boot.asm        (stage 1, 512 bytes, CHS read of loader2)
 → loader2.asm     (stage 2, 8192 bytes, LBA read of kernel, display policy)
 → kernel.bin      (loaded to 0x1000)
 → protected mode
 → kernel_entry.asm  (zeros BSS, calls kernel_main)
 → kernel_main()     (timestamped diagnostics, storage selection, boot log save)
 → bootseq task      (shell preload, HID/log save, late splash, shell resume)
```

---

# Disk Layout

The OS image (`smallos.img`) is structured as:

```text
LBA 0           → boot.bin              (512 bytes, exactly)
LBA 1–16        → loader2.bin           (currently 8192 bytes, exactly)
LBA 17+         → padded kernel region  (sector-aligned)
LBA 17+ks
               → ext2 partition        (16 MB volume inside the image)
```

where `ks = ceil(kernel.bin / BOOT_SECTOR_SIZE)` and `kernel_lba = 1 + loader2_sectors`.

Constraints:

* `boot.bin` must be **exactly `BOOT_SECTOR_SIZE` bytes**
* `loader2.bin` must be **exactly `LOADER2_SIZE_BYTES` bytes** (currently 8192 bytes / 16 sectors)
* `kernel.bin` must be **padded to a sector boundary during final image assembly** so the ext2 partition starts at a clean LBA
* ext2 partition starts at `kernel_lba + kernel_sectors`
* partition entry 0 records the kernel LBA range and boot flag
* partition entry 1 records the ext2 LBA range; stage 2 caches it before the kernel overwrites `0x7C00`

---

# Stage 1 – boot.asm

## Location

Loaded by BIOS at `0x0000:0x7C00`.

## Responsibilities

* Initialize segment registers and a temporary stage-1 stack at `0x9000`
* Print a compact, one-cell-inset status banner via BIOS `int 0x10`
* Load stage 2 from disk to `0x40000`
* Transfer control to stage 2

## BIOS Disk Read

Stage 1 uses the CHS interface for loader2 only. Loader2 is `LOADER2_SECTORS` sectors and fits within the first track.

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

## Early Screen Layout

Stage 1 clears the BIOS text screen, starts its status text at row 1 / column 1,
and keeps later lines aligned to that one-cell margin. The visible stage-1 text is
intentionally log-like rather than centered or framed:

```text
 SmallOS
 stage-1 bootstrap

 reading stage 2...
 stage 2 ready
```

Stage 2 uses the same VGA teletype margin helper for its real-mode status lines,
so `Loading kernel...` and `Preloading ext2 fallback...` stay inside the same
left buffer before the kernel display backend takes over. The COM1 serial mirror
does not include cosmetic leading spaces; it remains a compact transcript for
QEMU/ESXi smoke harnesses.

---

# Stage 2 – loader2.asm

## Location

Loaded to `0x4000:0x0000` (physical `0x40000`).

## Responsibilities

* Check INT 0x13 LBA extension support and print drive diagnostics
* Load kernel from disk immediately before the ext2 partition to physical `0x1000`
* Preload the used prefix of the ext2 partition through BIOS reads into a 16 MB
  zero-filled RAM fallback at `0x800000`
* Apply the build-time display policy: VBE framebuffer in auto mode, BIOS/VGA text in forced VGA mode
* Copy the BIOS 8x16 font, publish framebuffer fields when VBE is selected, and collect BIOS E820 memory-map entries in boot info
* Setup temporary GDT
* Switch to 32-bit protected mode
* Jump to kernel entry at `0x1000`

## Safe Kernel Size

Loader2 now sits at `0x40000`. The kernel loads to `0x1000`. Stage 2 now uses a loader-segment stack, so the real-mode `SP` is `0xFF00` while the physical stack top is `0x4FF00`. The kernel must not grow large enough to overwrite the loader during the INT 0x13 read:

```text
safe kernel size = (0x40000 - 0x1000) / 512 = 504 sectors = 252 KiB
```

That is still the practical envelope for the current layout, but the build no longer enforces a hard ceiling. If the kernel grows too large, stage 2 will overwrite itself while reading from disk. Any future layout bump must update both the loader placement and the stage-2 stack contract.

## Layout Ownership

The boot stages own the disk-layout constants they depend on:

* `boot.asm` declares `BOOT_SECTOR_SIZE`
* `boot.asm` declares `LOADER2_SECTORS`
* `boot.asm` declares `MBR_PARTITION_TABLE_OFFSET`
* `boot.asm` declares `MBR_PARTITION_ENTRY_SIZE`
* `loader2.asm` declares `LOADER2_SIZE_BYTES`

The Makefile reads these declarations during image construction rather than redefining the numbers independently, injects the generated stack-top values into `loader2.asm`, and passes the resulting layout facts into `mkimage` for final disk-image assembly.

## Layout Check

`make boot-layout-check` verifies the built boot artifacts and the generated loader template before the final image is assembled. It checks:

* `boot.bin` is exactly 512 bytes
* `loader2.bin` is exactly `LOADER2_SIZE_BYTES` bytes
* loader2 still loads at `0x40000`
* the generated stage-2 stack and the kernel boot stack match the current contract

This keeps the hand-rolled boot path explicit: the build fails before QEMU starts if the fixed boot-stage layout drifts.

`make image-layout-check` goes one step further and validates the finished `smallos.img` itself. It checks the partition table entries, the loader and kernel placement, the ext2 start LBA, and the zero padding between the kernel and ext2 region.

`make verify` runs the standard preflight sequence: boot-layout check, image-layout check, guest `test`, and `smoke`. Broader gates live behind `make verify-display`, `make verify-network`, and `make verify-full`.

**Symptom of violation:** BIOS INT 0x13 hangs silently mid-transfer. The screen shows `Loading...` but never advances. No error is printed because the hang occurs inside the BIOS call.

---

## Why LBA Extended Reads

Stage 2 prefers **INT 0x13 AH=0x42** (LBA extended read) for disk reads. LBA is
preferred because:

* The kernel and ext2 partition can easily extend beyond one CHS track (18 sectors on a standard floppy geometry).
* CHS reads beyond sector 18 on track 0 either fail or silently read wrong data.
* LBA addressing has no track geometry limit.

**The system should be launched as a hard disk** (`-drive format=raw,file=...` in QEMU).
When a BIOS does not report LBA extensions, stage 2 prints diagnostics and
attempts a CHS fallback.

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

If not supported, loader2 prints `NO LBA; CHS fallback` plus the BIOS drive
number and CHS geometry, then attempts classic INT 0x13 sector reads.

## USB Boot Storage

On BIOS USB boot, the firmware can read sectors while the bootloader is still in
real mode, but the protected-mode kernel cannot assume that the USB stick exists
as primary ATA hardware. By default, stage 2 now treats this as a live USB block
boot: if EDD identifies the boot drive as USB, it skips the ext2 RAM fallback and
lets the protected-mode USB mass-storage driver mount the stick through `usb0`.
For non-USB BIOS disks, stage 2 still preloads the used ext2 prefix into RAM and
publishes the 16 MB zero-filled fallback through boot info. It prefers INT 0x13
LBA reads and falls back to CHS reads when the BIOS reports no LBA extensions.

The protected-mode kernel now has three storage choices:

* writable ATA via the generic block-device wrapper
* read-only USB mass storage through OHCI + Bulk-Only Transport + SCSI READ(10)
* the loader2 RAM fallback

The mount order is ATA first, USB storage second, and boot RAM fallback last.
QEMU and VMware IDE-style boots keep the writable ATA path. Real USB boots can
mount the same on-stick ext2 volume through `usb0`; writes are disabled on that
path until USB storage write support exists. The RAM fallback remains useful
when protected-mode storage cannot validate the disk.

`BOOT_RAMDISK_FALLBACK=auto` is the default. `always` / `1` forces the old
preload path, and `never` / `0` disables publication of the loader2 RAM fallback
for all boot devices. The explicit USB image/run targets set `always` so direct
USB builds remain bootable on hardware whose protected-mode USB storage path
cannot yet validate ext2.

During early storage probing, the kernel temporarily unmasks only timer IRQ0.
That keeps `[ms=... tick=... cyc=...]` boot timestamps advancing while avoiding
keyboard/mouse/process IRQ delivery before the scheduler owns a current task.

After the filesystem is mounted and the user shell image has been loaded, the
boot sequence probes OHCI boot HID devices. The shell process stays suspended
until this first HID pass finishes, so the serial log captures either
`usb: boot keyboard port=N`, `usb: boot mouse port=N`, or
`usb: WARN boot HID unavailable` before the first prompt. A failed first pass is
not final: the `usb` service task retries boot keyboard and mouse discovery
once per second, skips the already-mounted USB-storage port and already-active
HID ports, and preserves USB addresses for failed attempts so late devices can
still be claimed after the shell is visible. The HID matcher accepts keyboard
and mouse protocol interfaces even when older firmware reports a non-boot HID
subclass, then requests boot protocol before polling. The visible `Input:` lines show
keyboard and mouse endpoint/poll/report counters so hardware bring-up can be
debugged even when only one input device is working.

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
kernel_lba, kernel_sectors
ES:BX = 0x0000:0x1000  →  physical 0x1000
```

`loader2.asm` reads the kernel LBA and size from partition entry 0 at runtime. There are no injected kernel-size placeholders in the stage-2 binary.

---

## Injected Values

The Makefile still generates `build/gen/loader2.gen.asm`, but only the stack-top values are injected:

```text
__STAGE2_STACK_TOP__    0xFF00
__STAGE2_STACK_TOP_32__ 0x1FF000
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

Loader2 must be exactly `LOADER2_SIZE_BYTES` bytes. The source enforces a fixed-size binary layout, stage 1 loads `LOADER2_SECTORS`, and the Makefile/verifiers ensure those values agree.

## Boot Info

Before entering protected mode, loader2 clears and writes the versioned boot
info block at `0x90000` with magic `SMOS`, version 3, framebuffer fields, and
up to 32 BIOS E820 memory-map entries. E820 is best-effort: if INT 15h E820
fails, `e820_valid` remains zero and the kernel boots with the fixed PMM
fallback.

Loader2 also normally queries VBE, scans for a 1024x768x32 graphics mode with
a linear framebuffer, copies the BIOS 8x16 font to `0x91000`, and publishes
framebuffer fields in the boot info block. If any VBE step fails,
`framebuffer_valid` remains zero and the kernel keeps the VGA text backend.
With `DISPLAY_BACKEND=vga`, loader2 keeps BIOS/VGA text mode while still
collecting E820.

Loader2 enables A20 before switching to protected mode. The kernel immediately
uses memory above 1 MiB for `.bss` and the boot stack, so relying on BIOS/QEMU
defaults can triple-fault on firmware that leaves A20 disabled. ESXi is one
environment where making this explicit matters.

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

BSS zeroing is the first thing that happens in protected mode. The linker
places `.bss` at `0x100000` as a `NOLOAD` section, so these zero bytes are not
part of `kernel.bin` and cannot overwrite loader-owned low memory such as boot
info at `0x90000`. The kernel's three paging structures
(`kernel_page_directory`, `low_page_table_0`, `low_page_table_1`) are static
arrays in `.bss`. Without zeroing they contain garbage and `paging_init()`
immediately triple-faults.

`bss_start` and `bss_end` are linker symbols from `linker.ld`. Both must be declared `extern` in the asm file.

---

# Memory Map During Boot

```text
0x00007C00   stage 1 (boot.asm) — done after jump to 0x40000
0x00040000   stage 2 (loader2.asm) — done after jump to 0x1000
0x00001000   kernel entry point (_start in kernel_entry.asm)
0x00090000   loader-written boot info
0x00091000   copied BIOS font
0x00100000   kernel .bss start (NOLOAD; zeroed by kernel_entry.asm)
~0x00190000  current kernel .bss end; varies with static buffers
~0x00190000  bump allocator start, page-aligned from bss_end
0x001FF000   stack top (set up by loader2's init_pm)
```

---

# Why Two Stages Exist

Stage 1 cannot fit LBA logic, protected mode setup, or disk reads in 512 bytes. Stage 2 carries all of that complexity.

| Stage   | Purpose                                                      |
| ------- | ------------------------------------------------------------ |
| Stage 1 | load fixed-size stage 2 via CHS                              |
| Stage 2 | LBA extension check, kernel load, display policy, protected mode |

---

# Kernel Padding — Critical Design Constraint

`kernel.bin` is padded to a `BOOT_SECTOR_SIZE` boundary during final image assembly by `mkimage`.

Without this padding, the ext2 partition does not start on a clean sector boundary. Final image assembly computes `kernel_lba = 1 + loader2_sectors` and `ext2_LBA = kernel_lba + kernel_sectors`, where `kernel_sectors = ceil(kernel.bin / BOOT_SECTOR_SIZE)`. If `kernel.bin` is not padded before `ext2.img` is appended, ext2 reads land mid-sector and return incorrect data.

---

# ext2 LBA Delivery

The ext2 partition start LBA cannot be a compile-time constant in `ext2.c` without a chicken-and-egg dependency (kernel compilation needs it, but it depends on kernel size). Instead:

1. The Makefile discovers source-owned layout constants from `boot.asm` and `loader2.asm`
2. `mkimage` computes `kernel_lba = 1 + loader2_sectors` and then `ext2_lba = kernel_lba + kernel_sectors` during final image assembly
3. `mkimage` writes the kernel and ext2 spans into the MBR partition table entries declared by `MBR_PARTITION_TABLE_OFFSET` and `MBR_PARTITION_ENTRY_SIZE` in `boot.asm`
4. At runtime, `ext2_init()` normally reads ATA sector 0 and extracts the partition metadata; if that mount fails, the kernel tries USB storage; if that also fails and loader2 published the preloaded ext2 fallback, the kernel retries against the RAM-backed volume

The partition table lives in the declared boot-sector padding area before the `0x55AA` signature and is safe to overwrite.

---

# Bootloader Contract

The following must always hold:

* kernel starts at `kernel_lba = 1 + loader2_sectors` in the disk image
* kernel is loaded to physical `0x1000`
* `kernel.bin` is padded to a whole-sector boundary during final image assembly
* ext2 partition starts at `kernel_lba + kernel_sectors` (LBA)
* partition entry 0 describes the kernel range
* partition entry 1 describes the ext2 range
* loader2 GDT is temporary — kernel installs its own GDT first
* segment registers correctly initialized before protected mode entry
* `bss_start` / `bss_end` correctly defined and BSS zeroed before `paging_init()`
* `.bss` remains `NOLOAD` and starts above loader-owned low memory; current
  `kernel_selfcheck()` requires the initial bump pointer to stay below the boot
  stack page
* `0x1000 + kernel_sectors * 512` must stay below both the loader2 load address and the generated stage-2 stack top

Violation of any of these results in an immediate crash or silent corruption.

---

# Failure Modes

## Hangs at "Loading..." with no error

The kernel load overwrote loader2 or the generated stage-2 stack mid-transfer. The kernel has grown beyond the generated stage-2 ceiling. Move stage 2 higher and update the matching constants in `Makefile` and `loader2.asm`.

## "Disk read error!" on screen

The active BIOS disk read returned carry set. Causes:

* Drive not presented as hard disk — use `-drive format=raw,file=...` not `-fda`
* LBA address out of range — disk image too small or ext2.img not appended
* CHS fallback geometry does not match the BIOS's USB-disk translation

## "NO LBA; CHS fallback" on screen

BIOS does not report INT 0x13 extensions for the boot drive. Record the whole
diagnostic line, especially `drive=`, `heads=`, and `spt=`.

## VMware console accepts clicks but not movement

ESXi browser consoles can deliver mouse buttons through the PS/2 IRQ path while
movement arrives through VMware's absolute-pointer backdoor. SmallOS handles
that VMware path in `src/drivers/mouse.c` by draining the absolute-pointer queue
on IRQ12 and converting absolute positions into relative `SYS_MOUSE_READ`
deltas. Avoid VMX `mouse.*` or `vmmouse.*` overrides on ESXi 6.7 unless you are
deliberately testing VM configuration; the known-good baseline leaves those
keys absent and keeps `usb.present = "FALSE"`.

Use `/bin/mousetest.elf` through the shell command `mousetest` to confirm input
delivery. A VMware console with working movement should report nonzero
`dx`/`dy` events and a summary with nonzero `vmware=` packets.

## "ext2: bad MBR signature", "ext2: MBR partition type mismatch", or "ext2: partition entry not populated"

The partition table is missing, malformed, or the ext2 image was not appended. Check the `mkimage` step in the final image build.

## Triple fault immediately after kernel loads

BSS not zeroed before `paging_init()`. Confirm:

* `kernel_entry.asm` contains the `rep stosb` BSS zeroing loop
* `extern bss_start` / `extern bss_end` declared in `kernel_entry.asm`
* `bss_start = .;` / `bss_end = .;` present in the `NOLOAD` `.bss` section of
  `linker.ld`

## Reboot loop

Typical causes: invalid kernel GDT (must call `gdt_init()` before `sti`), invalid IDT, broken interrupt handler.

## Loader2 assembly error: binary too large

NASM rejects the `times LOADER2_SIZE_BYTES-($-$$) db 0` padding because the code exceeds the declared fixed size. Shorten message strings, remove debug code, or intentionally raise both `LOADER2_SIZE_BYTES` and `LOADER2_SECTORS`.

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
qemu-system-i386 -drive format=raw,file=build/img/smallos.img \
    -no-reboot -no-shutdown \
    -d int,cpu_reset,guest_errors \
    -D qemu.log
```

## QEMU USB Storage Path

Use the dedicated target to boot the canonical raw image as an OHCI USB storage
device:

```bash
make run-usb-storage
make usb-storage-smoke
```

The smoke target expects the serial transcript to show `usbms: ready`,
`dev=usb0`, and `boot: PASS ext2: volume mounted from USB`. The QEMU fixture
does not attach a boot keyboard, so `usb: WARN boot HID unavailable` is
acceptable as long as `usb: HID service task queued` follows it; on hardware the
same service continues retrying until it logs `usb: boot keyboard port=N` and,
when a boot mouse is attached on a supported OHCI root port, `usb: boot mouse
port=N`.

---

# Design Decisions

## Partition Table Layout

Pros: explicit and standard, without relying on ad hoc boot-sector patch fields.
Cons: a little more code in stage 2 and the kernel.

## Fixed Load Address (0x1000)

Pros: easy to reason about, matches linker script directly.
Cons: no relocation support.

## ext2 LBA via Partition Entry

Pros: no compile-time dependency, no chicken-and-egg; the correct value is always in the image regardless of kernel size changes.
Cons: the image builder and consumers need to agree on partition-table offsets and entry meanings.

## Storage Preference

Programs are loaded from the mounted ext2 partition at runtime. The kernel
tries ATA first because it is writable, then USB mass storage as a read-only
block device, then the loader2-published RAM fallback. With the default
`BOOT_RAMDISK_FALLBACK=auto` policy, loader2 skips that preload for BIOS USB
boot drives and keeps it for non-USB BIOS disks. USB image targets override this
to `always` because real USB firmware and controllers are less predictable than
QEMU.

USB HID is deliberately claimed after storage. That keeps shell ELF loading on
the storage path that just mounted, then starts a retrying OHCI boot-HID service
before the shell process is released. Keyboard input enters the normal keyboard
consumer path as injected set-1 scancodes; no separate shell routing exists for
USB keyboards. USB mouse packets enter the normal mouse accumulator, so
`SYS_MOUSE_READ` and the higher-level input queue see the same movement state as
PS/2 and VMware mouse input.

---

# Summary

```text
Stage 1  →  load stage 2 (CHS, fixed-size loader, to 0x40000)
Stage 2  →  LBA extension check
         →  read partition table metadata at 0x7C00
         →  derive kernel_lba from partition entry 0
         →  load kernel to 0x1000
         →  collect boot info and BIOS E820 memory map at 0x90000
         →  enable A20
         →  protected mode entry
Kernel   →  zero BSS
         →  terminal_init, gdt_init, paging_init
         →  memory_init(page-aligned bss_end), pmm_init
         →  keyboard, mouse, timer, idt, sched_init
         →  ata_init, pci_init, e1000_init
         →  dhcp_configure (best-effort IPv4 lease; runtime config can later be inspected or replaced with ip/ipconfig)
         →  tcp_init
         →  ntp_sync (best-effort realtime clock sync through DHCP gateway)
         →  mount ext2 from ATA, USB storage, or boot RAM fallback
         →  save /var/log/boot.txt when the filesystem is writable
         →  create bootseq task and zombie reaper, sched_start with IF masked
Bootseq  →  load /bin/shell.elf suspended
         →  enable interrupts in kernel-task bootstrap
         →  probe OHCI boot keyboard/mouse HID and queue retrying usb service
         →  refresh /var/log/boot.txt
         →  run /bin/bootsplash.elf boot/splash.bmp
         →  print SmallOS ready
         →  launch /bin/shell.elf
         →  idle if the user shell exits or fails
```

Boot code is the most fragile part of the system. Failures here are often silent — no terminal, no debug output, just a reboot loop or a hung screen.

---

# Future Work

* Support kernels larger than the current loader2/stack envelope
* Multiboot-style boot protocol for GRUB compatibility
