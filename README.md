# SmallOS

![SmallOS boot splash](assets/boot_splash.jpg)

SmallOS is a BIOS-booted 32-bit x86 hobby operating system. It builds a raw
hard-disk image, boots through a two-stage loader, enters protected mode,
enables paging, mounts an ext2 filesystem, and runs a small shell plus ring-3
ELF programs.

The project is intentionally small enough to understand end to end, but it now
has real subsystems: process scheduling, user/kernel syscalls, persistent disk
state, framebuffer graphics, TCP services, and a hosted TinyCC build inside the
guest.

## Highlights

- BIOS boot from a raw disk image:
  stage 1 loads stage 2 with CHS, stage 2 uses LBA reads for the kernel.
- 32-bit protected-mode C kernel with its own GDT, IDT, TSS, paging setup, and
  page-fault handling.
- E820-aware physical memory manager plus a simple kernel heap for permanent
  kernel allocations.
- Preemptive round-robin scheduler with kernel tasks, ring-3 ELF processes,
  per-process address spaces, per-process kernel stacks, `fork`, `execve`,
  legacy spawn-style `exec`, `waitpid`, `yield`, zombie reaping, and
  user-fault isolation.
- `int 0x80` syscall ABI for console I/O, files, directories, cwd, process
  control, pipes, descriptor duplication, heap growth, time, framebuffer
  display, input, sockets, polling, and timer/signalfd-style shims.
- ATA and USB mass-storage block devices with an ext2-backed VFS. ATA is
  writable; USB BOT/SCSI storage is mounted read-only today. The generated
  filesystem includes `/bin`, `/usr/bin`, `/usr/sbin`, `/usr/libexec/tests`,
  `/etc`, `/boot`, `/var`, and `/tmp`, with boot diagnostics persisted at
  `/var/log/boot.txt` when the mounted filesystem is writable.
- Framebuffer terminal with VGA text fallback, boot timing prefixes captured in
  `/var/log/boot.txt`, graphical boot splash that covers final startup work,
  PS/2 keyboard, retrying OHCI USB boot keyboard/mouse probing, PS/2 plus
  VMware mouse input, and several graphics demos.
- PCI networking with e1000 and RTL8139 NIC support, DHCP, ARP, IPv4, UDP/NTP clock sync,
  runtime `ip`/`ipconfig` inspection and configuration, a compact TCP service
  task, passive sockets, `poll`/`epoll` readiness, FTP, echo, and HTTP server
  smoke paths.
- Guest userland includes familiar commands such as `ls`, `tree`, `cat`,
  `more`, `man`, `pwd`, `touch`, `mkdir`, `rm`, `cp`, `mv`, `edit`, `date`, `ip`,
  `ipconfig`, `uptime`, `halt`, and `reboot`, plus diagnostics such as
  `cpuz`, `usbinfo`, `usbports`, `usbpower`, `mousetest`, and demos.
- TinyCC is built as `usr/bin/tcc.elf` and can compile sample C programs inside
  SmallOS.

## Requirements

Build tools:

```text
nasm
i686-elf-gcc
i686-elf-ld
i686-elf-objcopy
gcc
python3
qemu-system-i386
```

Third-party package sources are git submodules:

```bash
git clone --recurse-submodules <repo-url>
cd SmallOS
```

If the repository was cloned without submodules, run:

```bash
make deps
```

## Build And Run

Build the canonical artifacts:

```bash
make clean && make
```

This writes `build/img/smallos.img` and `build/img/smallos.vmdk`. QEMU boots
the raw image directly. For hardware USB testing, `make usb-image` refreshes
the stable burn target at `build/usb/smallos-wyse-s10-direct-usb.img`.
Build-profile directories include the display backend, serial mode, and NIC
selection, for example `build/bin/auto-serial-e1000/`. The seeded ext2 image is
built under `build/bin/<profile>/ext2.seed.img`, then
copied to the mutable runtime partition at `.state/ext2.img`. Guest-created
files survive normal rebuilds, while the `.state/ext2.img.stamp` dependency
lets Make refresh the runtime partition when userland binaries or seeded manual
pages change. Reset it from the current seed image with:

```bash
make reset-disk
```

Write the USB image to a whole device, not a partition:

```bash
sudo dd if=build/usb/smallos-wyse-s10-direct-usb.img of=/dev/sdX bs=4M conv=fsync status=progress
```

To rebuild only the raw image:

```bash
make image
```

To rebuild only the VMware/ESXi wrapper from the same raw image:

```bash
make vmdk
```

Interactive runs:

```bash
make run       # QEMU curses display, default
make run-gtk   # graphical GTK display
make run-sdl   # graphical SDL display
```

`make run` uses QEMU user-network NAT with an e1000 NIC by default, and the guest acquires
its IPv4 configuration with DHCP. `make run-usb-storage` boots the same raw
image through QEMU OHCI USB mass storage, which exercises the protected-mode
USB storage path instead of the IDE disk path. The USB image/run targets keep
the loader2 RAM fallback enabled for hardware safety while the kernel still
prefers the live `usb0` mount when it validates. If terminal input feels
sluggish through curses, use `make run-gtk` or `make run QEMU_DISPLAY=gtk`.
Mouse-driven graphics demos need a graphical QEMU backend and a grabbed QEMU
window.

Headless run with serial logging:

```bash
make run-headless
tail -f /tmp/smallos-serial.log
```

Headless QEMU writes its PID and monitor socket to:

```text
/tmp/smallos.pid
/tmp/smallos-monitor.sock
```

Use `QEMU_MEMORY_MB=128` to exercise more of the PMM-managed memory range.

## Networking

The default run and test paths use QEMU user-network NAT. Host forwarding can
be passed through `QEMU_NET_HOSTFWD`; for example, this forwards host port
2323 to the guest echo service port:

```bash
make run-headless \
  QEMU_NET_HOSTFWD=',hostfwd=tcp::2323-:2323'
```

Inside the guest, the FTP and web services start by default:

```text
usr/sbin/ftpd --quiet
usr/sbin/cserve --port 8080 --root /var/www --max-conn 28 --log off
```

Additional or replacement services can still be launched from the shell:

```text
bg usr/sbin/tcpecho
bg usr/sbin/ftpd --log-file /var/log/ftpd.log
bg usr/sbin/cserve --config /etc/cserve.ini
```

Shell job control supports `jobs`, `fg <jobid>`, Ctrl+Z, and `kill <jobid>`.
Manual `ftpd` launches write service output to `/var/log/ftpd.log`; manual
`cserve` launches use the log path from `/etc/cserve.ini`.

For TAP networking, create and configure the TAP interface on the host first,
then run:

```bash
make run-tap QEMU_NET_IFACE=tap0
make run-headless-tap QEMU_NET_IFACE=tap0
```

One simple Linux TAP setup is:

```bash
sudo ip tuntap add dev tap0 mode tap user "$USER"
sudo ip link set tap0 up
sudo ip addr add 192.168.100.1/24 dev tap0
```

Bridge or route that interface if the guest should reach beyond the host.

Inside SmallOS, `ip` and `ipconfig` show and update the runtime IPv4
configuration:

```text
ip
ip addr add 192.168.100.2/24 gateway 192.168.100.1 dns 1.1.1.1
ip route add default via 192.168.100.1
ip dns set 1.1.1.1
ip dhcp
ipconfig /all
```

VMware ESXi deploys use the same VMDK and the same DHCP/NIC path:

```bash
make esxi-smoke ESXI_SMOKE_FLAGS="--host 10.10.0.13"
```

That target builds, uploads, replaces the VM disk, reboots, waits for
`SmallOS ready`, and checks the VMware boot markers. See `docs/build.md` for
the baseline VM shape and lower-level deploy/log helpers.

## Verification

Fast regression path:

```bash
make test
```

`make test` boots headlessly, checks the boot diagnostics, runs the shell
selftest, drives the interactive `readline` prompt, and verifies shipped ELFs
against expectations under `tests/shell/` and `tests/elfs/`.

Useful verification targets:

```bash
make verify          # layout checks, guest regression suite, reboot/halt smoke
make verify-display  # framebuffer and forced-VGA screenshot checks
make verify-network  # socket EOF/parallel, FTP, FTP loop, cserve
make verify-full     # all verification targets
```

Focused smoke targets are also available:

```bash
make smoke
make smoke-reboot
make smoke-halt
make display-smoke
make usb-storage-smoke
make socket-eof-smoke
make socket-parallel-smoke
make ftp-smoke
make ftp-loop-smoke
make cserve-smoke
```

## Repository Layout

```text
.
├── assets/          boot splash source/rendered assets
├── docs/            subsystem notes and deeper design docs
├── patches/         third-party patches applied in build-local copies
├── samples/         files seeded into the guest filesystem
├── src/
│   ├── boot/        BIOS stage 1, stage 2, kernel entry, ELF embedding helper
│   ├── drivers/     display, input, block, ATA/USB storage, ext2, PCI, net
│   ├── exec/        ELF loader
│   ├── kernel/      memory, paging, process, scheduler, syscall, VFS, time
│   ├── shell/       shell, parser, line editor, built-in commands
│   └── user/        user commands, demos, tests, runtime headers and libc-ish code
├── tests/           guest shell and ELF expectation files
├── third_party/     TinyCC, FTP packages, and cserver submodules
├── tools/           image builders, layout checks, QEMU test harnesses
├── Makefile
└── linker.ld
```

Generated artifacts live under `build/`. Persistent guest disk state lives
under `.state/`.

## Documentation

The README is meant to be the front door. The detailed notes live in `docs/`:

- [Build system](docs/build.md)
- [Boot process](docs/boot.md)
- [Architecture](docs/architecture.md)
- [Execution and scheduling](docs/execution.md)
- [Memory](docs/memory.md)
- [Filesystem](docs/filesystem.md)
- [Syscalls](docs/syscalls.md)
- [User runtime](docs/user-runtime.md)
- [Socket subsystem](docs/socket-subsystem.md)
- [Interrupts](docs/interrupts.md)
- [Development notes](docs/development.md)

## Disk Image Shape

The final image is assembled as:

```text
LBA 0       boot sector / MBR
LBA 1-16    stage-2 loader
LBA 17+     sector-padded kernel
after that  mutable ext2 partition
```

The boot sector stores MBR-style entries for the kernel region and the ext2
partition. Stage 2 reads the kernel location from the image metadata; the
kernel mounts ext2 through the first storage path that validates: writable ATA,
read-only USB mass storage, then the loader2-published RAM fallback. The default
`BOOT_RAMDISK_FALLBACK=never` policy skips the fallback preload for normal
VM/IDE boots. `BOOT_RAMDISK_FALLBACK=auto` preloads only when EDD does not
identify the boot drive as USB or ATA, and the explicit USB image/run targets
force it on so hardware boots remain recoverable when protected-mode USB
storage is not happy yet. USB EDD boots probe and byte-check direct high-memory
reads before using them; otherwise the loader falls back to its low-memory
bounce buffer. Boot diagnostics are captured with `[ms=... tick=... cyc=...]`
prefixes in `/var/log/boot.txt`; display output is muted once the protected-mode
kernel owns the terminal, then the bitmap splash is shown as soon as the shell
has been preloaded and remains visible until the welcome block and shell prompt
replace it. DHCP, NTP, and default services continue asynchronously during that
covered window, and their quiet-path messages are still appended to the boot
log. `make boot-layout-check`, `make image-layout-check`, and
`make usb-storage-smoke` keep those contracts honest before hardware runs.
