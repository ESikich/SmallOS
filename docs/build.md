# Build System

This document defines how the system is built, how artifacts are generated, and how the disk image is constructed.

---

# Overview

The build process produces:

```text
build/img/smallos.img
build/img/smallos.vmdk
```

This image contains:

```text
boot.bin         stage 1 bootloader   (size declared by boot.asm; currently 512 bytes)
loader2.bin      stage 2 loader       (size declared by loader2.asm; currently 8192 bytes)
kernel.bin       kernel               (padded to sector boundary during final image assembly)
.state/ext2.img ext2 partition     (mutable fixed-size ext2 volume, stored after the padded kernel)
```

---

# Toolchain

```text
nasm             → assembly (boot stages, interrupt stubs, kernel entry)
i686-elf-gcc     → C compilation (freestanding, 32-bit, no stdlib)
i686-elf-ld      → linking
i686-elf-objcopy → strip ELF metadata → flat binary
gcc              → host tool compilation (mkext2, mkimage)
```

The `third_party/cserver`, `third_party/ftp_client`, `third_party/ftp_server`,
and `third_party/tinycc` directories are git submodules. A fresh clone should
include them with:

```bash
git clone --recurse-submodules <repo-url>
```

For an existing clone, run:

```bash
make deps
```

That target runs `git submodule update --init --recursive`. The normal build
checks for the key third-party source files and prints that command if they are
missing.

The TinyCC sources stay clean in `third_party/tinycc`. SmallOS applies
`patches/tinycc/smallos.patch` to a build-local copy under
`build/tinycc-smalos-src/` before compiling the guest `usr/bin/tcc.elf` variant.

---

# Build Output Structure

```text
build/
├── bin/<backend>/ → final binaries (kernel.elf, kernel.bin,
│                    usr/bin/*.elf, usr/libexec/tests/*.elf,
│                    ext2.seed.img, boot.bin, loader2.bin, tcc-smalos.elf)
├── obj/<backend>/ → object files and depfiles (.o, .d), mirrored by source subtree
├── gen/<backend>/ → generated source (loader2.gen.asm)
├── img/           → final disk image and wrappers (smallos.img, smallos.vmdk)
└── tools/         → host tools (mkext2, mkimage)
```

The generated seed image lives at `build/bin/<backend>/ext2.seed.img`; normal
runs use the mutable copy at `.state/ext2.img` so guest writes survive ordinary
rebuilds. Make tracks that copy with `.state/ext2.img.stamp`, so rebuilding
userland refreshes the runtime partition from the latest seed instead of
leaving stale ELFs in place. Use `make reset-disk` when you want to discard
guest writes explicitly.

---

# High-Level Build Flow

```text
source files (.c, .asm)
  ↓
object files (.o)
  ↓
kernel.elf          usr/bin/hello.elf   usr/libexec/tests/*.elf   ...
  ↓                      ↓
kernel.bin         user program ELFs
  ↓                      ↓
loader2.gen.asm     ext2.seed.img      ← built by build/tools/mkext2 from seeded entries
  ↓                      ↓
loader2.bin          boot.bin
          \            |            /
           \           |           /
            \          |          /
                 mkimage
                   ↓
              smallos.img
```

`mkimage` performs the final disk-image assembly step. It pads `kernel.bin` to a whole number of sectors, computes the ext2 start LBA, concatenates the component binaries, and writes kernel/ext2 partition metadata into the MBR table fields declared by `boot.asm`.

`make boot-layout-check` verifies the generated boot-chain inputs before that step runs, and `make image-layout-check` verifies the finished image afterwards. The default `make` target builds both the raw image and the VMDK wrapper; use `make image` when you only want the raw image.

The public raw disk image is always `build/img/smallos.img`. Developer build
knobs such as `DISPLAY_BACKEND`, `SERIAL_CONSOLE`, and boot diagnostics can
still change the generated internals, but they no longer create separately
named release images.

## VMware ESXi VMDK

`make esxi-vmdk` refreshes `build/img/smallos.img`, runs the finished-image
layout check, then converts that same raw image to an IDE monolithic sparse
VMDK:

```bash
make esxi-vmdk
```

The default artifact is:

```text
build/img/smallos.vmdk
```

The default QEMU artifact is also kept current:

```text
build/img/smallos.img
```

By default the ESXi raw disk uses the assembled SmallOS image size. To pad the
raw disk before VMDK conversion, pass `ESXI_VMDK_SIZE`, for example:

```bash
make esxi-vmdk ESXI_VMDK_SIZE=64M
```

Use `DISPLAY_BACKEND=vga` only as a developer diagnostic when a VMware console
cannot display the VBE framebuffer path early enough. It still writes the same
public artifact names:

```bash
make esxi-vmdk DISPLAY_BACKEND=vga
```

The ESXi baseline VM should use legacy BIOS boot, an IDE disk, at least 32 MB
of RAM, and an e1000 virtual NIC. The SmallOS e1000 driver binds both QEMU's
Intel 82540EM device (`8086:100E`) and VMware's Intel 82545EM-style device
(`8086:100F`). Keep `usb.present = "FALSE"` and avoid manual `mouse.*` or
`vmmouse.*` VMX overrides on ESXi 6.7; SmallOS handles VMware absolute-pointer
events in the mouse driver and converts them into the normal `SYS_MOUSE_READ`
relative deltas. Serial logging is recommended because the browser console may
miss short framebuffer transitions during boot.

To import the generated VMDK on an ESXi datastore, upload it next to the VM and
clone it into VMFS format:

```sh
cd /vmfs/volumes/datastore1/SmallOS
vmkfstools -i smallos.vmdk smallos-vmfs.vmdk -d thin
```

The helper script automates the local build, upload, and remote `vmkfstools`
import over SSH:

```bash
ESXI_HOST=10.10.0.13 tools/deploy_esxi.sh --force
```

For password-based ESXi SSH, put the password in `ESXI_PASSWORD`. The script
uses `sshpass -e` when that variable is present:

```bash
ESXI_HOST=10.10.0.13 ESXI_PASSWORD='...' tools/deploy_esxi.sh --force
```

Or through make:

```bash
make esxi-deploy ESXI_DEPLOY_FLAGS="--host 10.10.0.13 --force"
```

To also replace the VM's IDE `0:0` disk and power it back on, pass
`--attach-and-reboot`. The script finds the VM by inventory name, defaulting to
`--vm-dir`/`SmallOS`:

```bash
ESXI_PASSWORD='...' tools/deploy_esxi.sh --host 10.10.0.13 --force --attach-and-reboot
```

The VM lifecycle flow is: upload the VMDK, find the VM, power it off if needed,
remove the configured disk slot without deleting its backing file, import the
new VMFS VMDK, attach it at the configured slot, reload the VM, and power it on.

Useful options include `--datastore`, `--vm-dir`, `--display-backend vga`,
`--password-env`, `--vm-name`, `--disk-controller`, `--disk-unit`,
`--controller-type`, `--skip-build`, and `--dry-run`.

Attach the cloned VMFS VMDK to the VM as an IDE disk, reload the VM if needed,
and boot from disk. The serial log should reach `SmallOS ready`.

For day-to-day VMware checks, use the serial-log helper. It resolves
`serial0.fileName` from the VMX unless `--serial-file` is provided:

```bash
make esxi-serial-log ESXI_SERIAL_FLAGS="--host 10.10.0.13 --tail"
make esxi-serial-log ESXI_SERIAL_FLAGS="--host 10.10.0.13 --follow"
```

The ESXi smoke helper clears the serial log, deploys the current VMDK,
replaces IDE `0:0`, powers the VM back on, waits for `SmallOS ready`, and checks
the VMware-relevant boot transcript markers:

```bash
make esxi-smoke ESXI_SMOKE_FLAGS="--host 10.10.0.13"
```

It expects the baseline VM shape above. Use `--skip-deploy` when you only want
to validate the current serial log, or `--display-backend vga` to smoke the
forced VGA image.

`make verify` is the standard preflight target: it runs both layout checks,
then `make test`, then `make smoke`. The heavier suites are grouped
separately: `make verify-display` runs the framebuffer/VGA visual smoke checks,
`make verify-network` runs the socket, FTP, and cserve smoke matrix, and
`make verify-full` runs all verification targets in sequence.

Most freestanding test ELFs define `_start(argc, argv)` directly and link with
the common user runtime objects. Hosted-ish programs define
`main(argc, argv, envp)` and link `src/user/user_crt0.c`, which supplies
`_start`, sets `environ`, and exits with `main`'s return value. `usr/bin/tcc.elf`
and `usr/libexec/tests/crtprobe.elf` use that CRT path; there is no
TinyCC-specific startup wrapper.

## Automated Guest Test

`make test` boots the finished image headlessly, verifies the boot
diagnostics splash markers, launches the shell `selftest` command, feeds
the interactive `readline` prompt, and verifies the built-in shell command
suite in `tests/shell/` plus every shipped ELF against the per-program
expectation files in `tests/elfs/`.

The scripted shell/selftest command tables are stored statically in the
kernel because the shell task's kernel stack is only 4 KB wide. That keeps
the long regression run from corrupting process state while the guest test
harness is exercising the full matrix.

The guest suite also exercises the SmallOS-hosted TinyCC compiler
(`usr/bin/tcc.elf`) by compiling several sample C files inside the guest
and immediately running the generated ELFs. `usr/bin/tcc.elf` links the generic
SmallOS CRT adapter and runs TinyCC's normal hosted CLI `main()` path inside
the freestanding guest runtime. Those generated binaries are stored under
`/var/tmp/`, while the shipped hello demo lives under `usr/bin/`.

The user runtime behavior that those tests depend on is documented in
[`docs/user-runtime.md`](user-runtime.md), including `errno`, cwd-aware
wrappers, stdio, directory traversal, and TinyCC expectations.

`make smoke` runs the dedicated reboot and halt smoke checks.  Use
`make smoke-reboot` or `make smoke-halt` to exercise those shell
commands on their own.

Use the aggregate targets as the normal verification ladder:

```bash
make verify          # layout, guest selftest, reboot/halt smoke
make verify-display  # framebuffer and forced-VGA screenshots
make verify-network  # socket EOF/parallel, FTP, FTP loop, cserve
make verify-full     # all of the above
```

`make framebuffer-smoke` boots the default display policy, waits for the
serial framebuffer boot marker and shell prompt, asks the QEMU monitor for a
PPM screenshot, and verifies that the image is a nonblank 1024x768 framebuffer.
`make vga-smoke` rebuilds with `DISPLAY_BACKEND=vga`, waits for the forced-VGA
serial marker, asks the QEMU monitor for a PPM screenshot, verifies that VGA
text output is visible, and fails if the framebuffer backend is selected
anyway. `make display-smoke` runs both. These visual checks use QEMU's VNC
display backend by default so the VM can stay daemonized while still rendering
screenshots. They are intentionally separate from plain `make test` because
screenshots depend more on the host QEMU display environment.

The display stack and user programs have separate optimization knobs:
`USER_CFLAGS` defaults to `-O2`, `DISPLAY_DRIVER_CFLAGS` defaults to `-O2` for
`display.o`, `fb_console.o`, `screen.o`, and `terminal.o`, while the broader
kernel remains controlled by `KERNEL_CFLAGS`.

`make socket-eof-smoke` boots QEMU with user-network host forwarding for
guest port `2463`, starts `usr/sbin/sockeof`, then verifies that
a 3072-byte multi-segment payload plus host half-close wakes guest `poll()`,
leaves the full payload readable, returns `0` on the next `read()`, and still
allows the guest to write back before `shutdown(SHUT_WR)` rejects later writes
and sends EOF to the host. The same smoke also verifies a guest-first half-close
through `close()` after a final guest write.

`make socket-parallel-smoke` forwards host port `2323` to guest `tcpecho`,
opens 8 parallel clients by default, sends small echo payloads over each
connection, holds the sockets briefly, and captures guest `netinfo` before,
during, and after the run. Override `SOCKET_PARALLEL_CLIENTS`,
`SOCKET_PARALLEL_ROUNDS`, or `SOCKET_PARALLEL_PORT` when needed.

`make ftp-smoke` boots QEMU with user-network host forwarding for FTP control
port `2121` and passive data port `30000`, starts `usr/sbin/ftpd`, then
drives login, negative path checks, `LIST`, `RETR`, `STOR` readback,
`DELE`, and `RMD` cleanup from the host.

`make ftp-loop-smoke` uses the same FTP forwards and repeats fresh control
sessions with passive `LIST`, `RETR`, `STOR`, uploaded-file readback, and
cleanup cycles. Override `FTP_LOOP_ITERATIONS` to change the loop count.

`make cserve-smoke` forwards host port `8080` to guest cserve, starts
`usr/sbin/cserve.elf --config /etc/cserve.ini`, fetches the large static
fixture with browser-shaped requests, holds keep-alive clients open, runs one
slow reader, checks a 404, and captures guest `netinfo` socket/TCP counters.
It holds 32 clients by default. Override `CSERVE_SMOKE_CLIENTS` or
`CSERVE_SMOKE_PORT` when needed.

`make usb-storage-smoke` boots the canonical raw image through QEMU OHCI USB
mass storage (`-device pci-ohci` plus `-device usb-storage`). It verifies that
ATA mount failure is tolerated, `usbms` reaches ready state, ext2 mounts from
`dev=usb0`, and the user shell prompt appears. That path is read-only today,
so boot log persistence is skipped with an informational boot message. The smoke
fixture intentionally has no USB keyboard; `usb: WARN boot HID unavailable`
followed by `usb: HID service task queued` means the retrying HID service is
alive for real hardware.

For networking, the default `run` and `test` targets keep using QEMU's
user-network NAT so CI stays simple. The guest still uses DHCP in that mode,
so the old QEMU `10.0.2.15/24` address is no longer hardcoded in the kernel.
`make run-tap` and `make run-headless-tap` switch the e1000 NIC over to a host
TAP device instead. That is the right path when you want the guest on a bridged
LAN or otherwise reachable beyond QEMU's built-in NAT layer.

Inside the guest, `ip`, `ipconfig /all`, `ip dhcp`, `ip addr add <addr>/<prefix>`,
`ip route add default via <gateway>`, and `ip dns set <server>` inspect or
replace the runtime IPv4 settings. Static settings use the same volatile kernel
network config as DHCP and are not written to disk.

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

Mouse-driven graphics demos need a graphical QEMU backend and a grabbed guest
window. Use `make run-gtk` or `make run-sdl`, then click/grab the QEMU window
before testing `usr/bin/mandel` cursor movement.

The guest terminal backend policy is controlled at build time:

```bash
make run-headless DISPLAY_BACKEND=auto  # default: VBE framebuffer, VGA fallback
make run-headless DISPLAY_BACKEND=vga   # force BIOS/VGA text mode
```

`DISPLAY_BACKEND=auto` keeps the normal VBE path: loader2 asks for
1024x768x32 and the kernel falls back to VGA text if VBE setup fails. By
default `BOOT_VBE_RELAXED=0`, so loader2 keeps scanning the BIOS mode list until
it finds the requested 1024x768x32 framebuffer instead of accepting an earlier
lower-resolution linear framebuffer. Set `BOOT_VBE_RELAXED=1` only when testing
firmware that cannot provide the preferred mode.
`DISPLAY_BACKEND=vga` keeps loader2 in BIOS/VGA text mode and defines
`SMALLOS_FORCE_VGA_BACKEND=1` for the kernel, so `fb_console_init()` returns
before mapping or selecting the framebuffer. The VGA panic and double-fault
paths remain available either way.

Before the kernel display backend takes over, both boot stages use plain BIOS
text output inset by one character cell from the top-left edge. The serial
mirror remains unpadded so host smoke logs keep compact line markers.

Serial console mirroring is enabled for normal builds so QEMU and ESXi smoke
checks observe the same boot transcript. Disable it only for visual-only
experiments:

```sh
make run SERIAL_CONSOLE=0
```

The headless test and smoke targets also pass `SERIAL_CONSOLE=1` explicitly
because their host harnesses use the serial log as the transcript. COM1-enabled
builds use `build/obj/auto-serial/` internally but still produce the canonical
`build/img/smallos.img`.

While boot diagnostics are being captured, terminal output is prefixed with
`[ms=... tick=... cyc=...]`. `ms` and `tick` come from the PIT tick counter;
`cyc` is a raw `rdtsc` sample. The storage probe briefly allows timer IRQ0 only
so those fields advance during ATA/USB probing without enabling keyboard or
mouse IRQ delivery before the scheduler is ready.

Userland framebuffer programs should use the small graphics helper in
`src/user/gfx.c`. It validates the display mode, acquires exclusive graphics
access, allocates a full-screen XRGB8888 backbuffer, and presents it with one
`SYS_DISPLAY_BLIT`.

QEMU guest RAM defaults to 32 MB. To exercise the expanded E820-backed PMM
window, override the memory size:

```bash
make test QEMU_MEMORY_MB=128
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
    -c file.c -o build/obj/<backend>/<subdir>/file.o
```

Each assembly file is assembled to ELF object form:

```bash
nasm -f elf32 file.asm -o build/obj/<backend>/<subdir>/file.o
```

C depfiles (`.d`) are emitted alongside object files so header changes rebuild the right targets automatically.

## Linking

All kernel objects are linked into `build/bin/<backend>/kernel.elf`:

```bash
i686-elf-ld -T linker.ld -m elf_i386 <objects> -o build/bin/<backend>/kernel.elf
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

    . = 0x100000;

    .bss (NOLOAD) : ALIGN(4096)
    {
        bss_start = .;
        *(.bss)
        *(COMMON)
        bss_end = .;
    }
}
```

`bss_start` and `bss_end` are used by `kernel_entry.asm` to zero the BSS
region at boot. `.bss` intentionally starts at `0x100000` and is `NOLOAD`: it
does not inflate `kernel.bin`, and runtime zeroing cannot clobber loader-owned
low memory such as boot info at `0x90000`. The PMM bitmap lives in BSS and must
be zero before `pmm_init()` — the BSS zeroing step handles this automatically.

## Binary conversion

```bash
i686-elf-objcopy -O binary kernel.elf kernel.bin
```

This strips all ELF metadata. The result is a flat binary. The `NOLOAD` `.bss`
section has no representation in this file — it is zero-initialized at runtime
by `kernel_entry.asm`.

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

tools/mkext2.c
  TOTAL_SIZE_MB
  TOTAL_SECTORS
```

This keeps the boot-stage source files and filesystem tool as the single source of truth for image-layout facts.

---

# User Programs (ELF)

User programs are compiled separately and packed into the ext2 seed image.
Adding or changing a program rebuilds the seed, refreshes the tracked mutable
copy at `.state/ext2.img`, and rebuilds the final disk image, but does not
require relinking `kernel.elf` or regenerating `kernel.bin` unless kernel
sources also change.

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
src/user/badptrprobe.c
src/user/preempt_test.c
src/user/fault.c
src/user/user_alloc.c
src/user/user_posix.c
```

All use `user_lib.h` and `user_syscall.h`. No libc, no hosted runtime, and no dynamic linking.

The guest compiler toolchain ships as `usr/bin/tcc.elf`, built from the TinyCC
submodule sources with the generic SmallOS CRT adapter. The guest entry point
bridges the kernel `_start(argc, argv)` launch ABI to TinyCC's normal
`main(argc, argv)` path. The shell selftests compile `usr/share/examples/tinycc/tccmath.c`,
`usr/share/examples/tinycc/tccagg.c`, `usr/share/examples/tinycc/tcctree.c`, and `usr/share/examples/tinycc/tccmini.c` inside the
guest with that compiler.

## Compile

```bash
i686-elf-gcc -I<dirs> \
    -ffreestanding -m32 -fno-pie -fno-stack-protector \
    -nostdlib -nostartfiles -MMD -MP -MF build/obj/auto/user/hello.d \
    -c hello.c -o build/obj/auto/user/hello.o
```

## Link

All user programs are linked at `USER_CODE_BASE` (0x400000):

```bash
i686-elf-ld -m elf_i386 -Ttext-segment 0x400000 -e _start \
    build/obj/auto/user/hello.o -o build/bin/auto/hello.elf
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

# ext2 Image

## Tool

`tools/mkext2.c` is a host C program compiled by the Makefile:

```makefile
$(TOOLS_DIR)/mkext2: tools/mkext2.c | dirs
    $(HOST_CC) -o $@ $<
```

## Building

```bash
build/tools/mkext2 build/bin/auto/ext2.seed.img \
    bin/echo.elf=build/bin/auto/echo.elf \
    bin/date.elf=build/bin/auto/date.elf \
    usr/bin/hello.elf=build/bin/auto/hello.elf \
    usr/bin/plasma.elf=build/bin/auto/plasma.elf \
    usr/bin/mandel.elf=build/bin/auto/mandel.elf \
    usr/libexec/tests/runelf_test.elf=build/bin/auto/runelf_test.elf \
    usr/sbin/ftpd.elf=build/bin/auto/ftpd.elf \
    usr/bin/tcc.elf=build/bin/auto/tcc-smalos.elf \
    usr/share/examples/tinycc/tccmath.c=samples/tccmath.c
```

Each `[dest=]source` argument either seeds the source file at its basename or
places it at an explicit ext2 destination path.  The Makefile expands this
into the full shipped image through `ext2_*_ENTRIES`; the command above is a
representative shape rather than the complete invocation.

`mkext2` produces a raw ext2 volume containing the shipped apps under
`bin/`, `usr/bin/`, `usr/libexec/tests/`, `usr/sbin/`, plus config/data
trees such as `/etc/`, `/var/`, and `/tmp/`. The image also seeds
`/var/log/boot.txt` from `samples/boot.txt`; the kernel overwrites that file
with the current boot diagnostics after ext2 mounts and again just before the
late boot splash is shown.

Shipped ext2 programs:
- `bin/echo` - print command arguments
- `bin/about` - print the OS version
- `bin/uptime` - print tick and second counts
- `bin/halt` - halt the machine
- `bin/reboot` - reboot the machine
- `bin/date` - print UTC realtime, or `date -s [server-ip]` to sync from NTP
- `bin/pwd` - print the process cwd inherited from the shell
- `bin/cat` - print an ext2 file
- `bin/more` - page an ext2 file or piped stdin
- `bin/fsread` - dump ext2 file metadata and first bytes
- `bin/ls` - list ext2 directories
- `bin/tree` - print an ext2 directory tree
- `bin/touch` - create or truncate an ext2 file
- `bin/rm` - remove an ext2 file
- `bin/mkdir` / `bin/rmdir` - create or remove ext2 directories
- `bin/cp` / `bin/mv` - copy or move ext2 entries
- `bin/bmpview` - load a BMP, render it into the `gfx` backbuffer, and present it to the framebuffer
- `bin/bootsplash` - non-interactive startup splash presenter for `/boot/splash.bmp`
- `bin/diskview` - show ext2 used/free space as a framebuffer allocation map
- `boot/splash.bmp` - splash BMP copied from `assets/boot_splash.bmp`
- `usr/bin/hello` - print argc/argv and tick count
- `usr/bin/plasma` - animated framebuffer graphics demo using `src/user/gfx.c`
- `usr/bin/mandel` - interactive Mandelbrot demo with arrow-key pan, +/- zoom, reset/quit keys, and mouse cursor movement
- `usr/libexec/tests/ticks` - print the current tick count
- `usr/libexec/tests/args` - print argc and argv
- `usr/libexec/tests/runelf_test` - verify ELF loading, syscalls, and stack setup
- `usr/libexec/tests/readline` - interactive SYS_READ demo
- `usr/libexec/tests/exec_test` - exercise SYS_EXEC semantics
- `usr/libexec/tests/waitprobe` - exercise getpid/waitpid/kill process lifecycle
- `usr/libexec/tests/pipeprobe` - exercise pipe EOF, EPIPE, nonblocking, PIPE_BUF, poll, and blocking transfer behavior
- `usr/libexec/tests/dupprobe` - exercise dup/dup2 shared descriptions and FD_CLOEXEC
- `usr/libexec/tests/forkprobe` - exercise fork memory copy, wait, and shared inherited fd offsets
- `usr/libexec/tests/execveprobe` - exercise replacing execve image handoff
- `usr/libexec/tests/fileread` - exercise VFS-backed file handles via SYS_OPEN / SYS_FREAD / SYS_CLOSE
- `usr/libexec/tests/compiler_demo` - exercise SYS_WRITEFILE, SYS_WRITEFILE_PATH, and readback
- `usr/libexec/tests/heapprobe` - exercise malloc/free/realloc/calloc
- `usr/libexec/tests/statprobe` - exercise SYS_STAT and path probing
- `usr/libexec/tests/fileprobe` - exercise file wrapper helpers, rename, unlink, and stat
- `usr/libexec/tests/cwdprobe` - exercise process cwd and relative path syscalls
- `usr/libexec/tests/stdioprobe` - exercise stdio EOF/error state, `clearerr`, and `fflush`
- `usr/libexec/tests/dirprobe` - exercise root and nested directory iteration
- `usr/libexec/tests/errnoprobe` - exercise raw syscall errors and POSIX errno wrappers
- `usr/libexec/tests/badptrprobe` - exercise unmapped user pointers, page-crossing buffers/structs, and wrapped syscall byte counts
- `usr/libexec/tests/sleep_test` - exercise SYS_SLEEP semantics
- `usr/libexec/tests/ptrguard` - exercise syscall pointer validation
- `usr/libexec/tests/spinwkr` - helper spawned by the preemption regression
- `usr/libexec/tests/preempt_test` - prove timer-driven preemption
- `usr/libexec/tests/crtprobe` - verify `main(argc, argv)` via `user_crt0`
- `usr/libexec/tests/fault` - fault probe (ud/gp/de/br/pf)
- `usr/bin/tcc.elf` - SmallOS-hosted TinyCC compiler binary linked through
  `src/user/user_crt0.c`
- `usr/share/examples/tinycc/tccmath.c`, `usr/share/examples/tinycc/tccagg.c`, `usr/share/examples/tinycc/tcctree.c`, `usr/share/examples/tinycc/tccmini.c` - guest compiler test inputs used by the shell selftests

## Properties

* fixed-size volume defined by `tools/mkext2.c`
* root directory is intended to stay directory-only during normal use
* `bin/` contains command-style app ELFs found by bare shell command lookup
* `usr/bin/` contains the hello and plasma demo ELFs
* `usr/libexec/tests/` contains the remaining shipped test ELFs
* `usr/sbin/` contains guest service ELFs
* `usr/bin/` contains the guest TinyCC binary and user-facing demos/tools
* `usr/share/examples/tinycc/` contains the shipped TinyCC sample inputs
* runtime-generated compiler outputs and scratch artifacts belong under `/var/tmp/`
* filenames are stored as native case-sensitive ext2 names
* no external filesystem tools are required

---

# Loader2 Generation

`loader2.asm` is generated at build time so the stage-2 stack-top values can be filled in without hardcoding them:

```asm
STAGE2_STACK_TOP    equ __STAGE2_STACK_TOP__
STAGE2_STACK_TOP_32 equ __STAGE2_STACK_TOP_32__
```

The Makefile injects those values from the generated stage-2 stack contract and writes `build/gen/<backend>/loader2.gen.asm` via `sed`:

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

`kernel.bin` must occupy a whole number of sectors in the final disk image before `ext2.img` is appended.

That padding is now performed by `mkimage`, not by a separate Makefile-generated `kernel_padded.bin` artifact.

**Why this is required:** The final ext2 start LBA is computed as:

```text
kernel_lba     = 1 + loader2_sectors
ext2_LBA      = kernel_lba + kernel_sectors
```

where:

```text
loader2_sectors = loader2.bin / BOOT_SECTOR_SIZE
kernel_sectors  = ceil(kernel.bin / BOOT_SECTOR_SIZE)
```

If `kernel.bin` is not padded to a sector boundary before `ext2.img` is appended, the ext2 volume would begin mid-sector in the final image while the kernel would still try to read it from the next full LBA. ext2 reads would then return incorrect data.

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
    --boot build/bin/auto/boot.bin \
    --loader build/bin/auto/loader2.bin \
    --kernel build/bin/auto/kernel.bin \
    --fs .state/ext2.img \
    --out build/img/smallos.img \
    --sector-size 512 \
    --loader-size 8192 \
    --boot-partition-table-offset 446 \
    --boot-partition-entry-size 16
```

## What `mkimage` does

`mkimage` assembles the final disk image as:

```text
boot.bin
loader2.bin
kernel.bin
zero padding to next sector boundary
ext2.img
```

It computes:

```text
loader2_sectors = loader2.bin / BOOT_SECTOR_SIZE
kernel_lba      = 1 + loader2_sectors
kernel_sectors  = ceil(kernel.bin / BOOT_SECTOR_SIZE)
ext2_LBA       = kernel_lba + kernel_sectors
```

It then writes the kernel and ext2 spans into the MBR partition table entries declared by `MBR_PARTITION_TABLE_OFFSET` and `MBR_PARTITION_ENTRY_SIZE` in `boot.asm`.

## Layout

```text
LBA 0                     boot.bin
LBA 1 ... loader2_sectors loader2.bin
LBA kernel_lba ... N      padded kernel region
LBA N+1 ...               ext2.img
```

The exact kernel span depends on `kernel.bin` size rounded up to a whole number of sectors.

---

# Stage 1 Bootloader

```text
boot.asm → build/bin/<backend>/boot.bin
```

Constraints:

* boot sector size is declared by `BOOT_SECTOR_SIZE` in `boot.asm`
* the BIOS boot signature `0xAA55` must be present at the end of the sector
* the boot sector contains MBR partition entries for the kernel and ext2 spans

Stage 1 uses the old CHS interface (`INT 0x13 AH=0x02`) because it only reads the fixed-size stage-2 loader, which fits comfortably within track 0.

---

# Running

```bash
make clean && make
qemu-system-i386 -drive format=raw,file=build/img/smallos.img
```

**Do not use `-fda`** (floppy disk mode). Floppy does not support INT 0x13 LBA extended reads. The system will halt with `NO LBA!` if launched as a floppy.

---

# Common Build Failures

## ext2: bad superblock magic (runtime)

Cause: ext2 start LBA was computed or patched incorrectly, ext2 image missing from the disk image, or launching in floppy mode. The kernel reads the wrong LBA or invalid data instead of an ext2 volume.

Fix: the `mkimage` step handles kernel padding and ext2 partition-table writing automatically. If the image is assembled manually, preserve the same layout and table rules.

## loader2.bin size error

```text
nasm: error: binary too large for `times' command
```

or

```text
ERROR: loader2.bin must be LOADER2_SIZE_BYTES bytes, got N
```

Cause: code + strings in loader2 exceed `LOADER2_SIZE_BYTES`. Remove or shorten debug message strings.

## Undefined `bss_start` / `bss_end` (NASM error)

```text
src/boot/kernel_entry.asm: error: symbol 'bss_start' not defined
```

Cause: symbols not declared `extern` in `kernel_entry.asm`, or not exported in `linker.ld`.

Fix: ensure `extern bss_start` / `extern bss_end` in `kernel_entry.asm`, and
`bss_start = .;` / `bss_end = .;` in the `NOLOAD` `.bss` section of
`linker.ld`.

## Triple fault on boot

Cause: kernel `.bss` not zeroed. Page tables and PMM bitmap contain garbage.
Verify `kernel_entry.asm` has the `rep stosb` loop and correct `extern`
declarations. Also verify `.bss` remains `NOLOAD` at `0x100000`; placing it
back in the low flat image can overwrite loader boot info.

## Disk read error (runtime, loader screen)

`INT 0x13 AH=0x42` returned carry set. Causes:

* launched as floppy (`-fda`) instead of hard disk — use `-drive format=raw`
* disk image too small — `ext2.img` not appended to image
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
usr/bin/hello.elf ────────────────────┐
usr/libexec/tests/*.elf ───────────────────────┤
usr/bin/tcc.elf / usr/share/examples/*.c ─┤→ ext2.seed.img → .state/ext2.img ─┐
                                        │                           │
kernel.bin ───────────────┐             │                           │
                          ├→ loader2.gen.asm → loader2.bin ───────┤
                          │                                        │
                          └────────────────────────────────────────┤
boot.bin ──────────────────────────────────────────────────────────┤
                                                                   │
mkimage ────────────────────────────────────────────────────────────┤
                                                                   ↓
                                                             smallos.img
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

## ext2 Image Instead of Embedded Programs

Current approach: ELF binaries are stored in a separate `ext2.img` partition. Kernel rebuild is not required to add or update programs. Kernel binary size is unaffected by program count.

## Generated Loader2

The Makefile generates `loader2.gen.asm` by text substitution into `loader2.asm`. This allows build-time injection of the generated stack-top values without hardcoding them.

## Host Tool Image Assembly

`mkimage` owns final disk-image assembly. This keeps Make focused on dependency orchestration while moving disk-layout mechanics (padding, LBA calculation, partition-table writing) into ordinary host-side code.

`make image-layout-check` is the companion verifier for the finished image. It checks that the assembled image still matches the intended sector map and partition-table layout.

## LBA Extended Reads

Replaces CHS `AH=0x02`. Removes the 18-sector-per-track limit. Required because the kernel and ext2 partition both live beyond what simple CHS assumptions can safely handle.

At runtime, the ATA driver uses PCI IDE bus-master DMA when QEMU exposes it and
falls back to the original polling PIO transfer path when DMA is unavailable or
fails.

## USB Storage Boot Smoke

QEMU can also expose the raw image as OHCI USB storage:

```bash
make run-usb-storage
make usb-storage-smoke
```

The kernel uses the generic block layer to try writable ATA first, then
read-only USB mass storage, then the loader2 RAM fallback when one was
published. With the default `BOOT_RAMDISK_FALLBACK=auto`, loader2 skips the
fallback preload for BIOS USB boot drives so USB boots mount the live `usb0`
device instead of paying the real-mode copy cost when firmware reports USB
through EDD. The explicit `usb-image`, `run-usb-storage`, and
`run-headless-usb-storage` targets force `BOOT_RAMDISK_FALLBACK=always`, which is
the reliable setting for USB-specific images while the protected-mode USB
storage path is still hardware-dependent. Use `BOOT_RAMDISK_FALLBACK=never` only
when intentionally testing a no-fallback USB boot. The USB storage driver
implements enough BOT/SCSI to enumerate the device, read capacity, and issue
READ(10) requests for ext2 blocks.

USB boot keyboard discovery is handled by the same OHCI driver after storage is
mounted and the shell ELF has been loaded. A failed first probe does not freeze
input discovery: the `usb` kernel task retries once per second until a boot
keyboard is found, while skipping the active USB-storage port.

---

# Future Improvements

* Broader DHCP coverage such as renewal and lease expiry handling
* Broader TCP close-state fuzzing beyond the focused EOF smoke
* Richer filesystem metadata such as long filenames, chmod, or ownership
