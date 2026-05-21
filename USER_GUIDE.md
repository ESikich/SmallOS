# SmallOS User Guide

This guide is for using SmallOS, not for studying how it is built inside. If
you want the kernel, bootloader, filesystem, scheduler, or syscall details, see
the technical notes under `docs/`.

## What SmallOS Is

SmallOS is a small 32-bit x86 operating system that boots into its own shell.
It has a writable filesystem, user programs, a text editor, manual pages,
network tools, a tiny C compiler, and simple FTP and HTTP services.

Most day-to-day use looks like this:

1. Build the disk image on your host machine.
2. Boot it in QEMU.
3. Use the SmallOS shell.
4. Create files, run programs, try networking, or compile a sample C program.

## Before You Start

Install the host tools:

```text
nasm
i686-elf-gcc
i686-elf-ld
i686-elf-objcopy
gcc
python3
qemu-system-i386
```

Clone the repo with submodules:

```bash
git clone --recurse-submodules <repo-url>
cd SmallOS
```

If you already cloned it without submodules, run:

```bash
make deps
```

## Build SmallOS

Build the normal disk image:

```bash
make clean && make
```

The main files created are:

```text
build/img/smallos.img
build/img/smallos.vmdk
```

For normal use you will boot `build/img/smallos.img` through the Makefile
targets below. For hardware USB testing, `make usb-image` also writes:

```text
build/img/smallos-wyse-s10-direct-usb.img
```

## Boot It

The default interactive run uses QEMU's curses display:

```bash
make run
```

Graphical backends are often nicer for demos, mouse input, and framebuffer
programs:

```bash
make run-gtk
make run-sdl
```

For a headless run with serial logging:

```bash
make run-headless
tail -f /tmp/smallos-serial.log
```

After boot, SmallOS prints a welcome block and drops you at its shell prompt.
The shell starts in `/`.

## Your First Commands

Try these inside SmallOS:

```text
help
about
pwd
ls
tree /
man smallos
man shell
```

`help` shows the built-in shell commands. `man` opens installed manual pages.
Manual pages are plain text and live in `/usr/share/man`.

## Moving Around The Filesystem

Useful filesystem commands:

```text
pwd
ls
ls /usr/bin
tree /var
cd /var/tmp
cat /var/www/index.html
more /var/www/index.html
```

SmallOS keeps normal-looking directories:

```text
/bin                 everyday commands
/usr/bin             demos and development tools
/usr/sbin            service programs
/usr/libexec/tests   diagnostic test programs
/usr/share/man       manual pages
/var/log             boot and service logs
/var/tmp             scratch files
/var/www             sample web content
```

## Creating And Editing Files

The ext2 filesystem is writable in normal QEMU runs. A small editing session:

```text
cd /var/tmp
touch notes.txt
edit notes.txt
cat notes.txt
cp notes.txt notes-copy.txt
mv notes-copy.txt renamed.txt
rm renamed.txt
```

Inside `edit`, press `F2` to save and `F3` or `Esc` to leave. Use `man edit`
for the editor's command details.

Guest-created files are stored in `.state/ext2.img` on the host and normally
survive rebuilds. To discard guest changes and return to the freshly seeded
filesystem:

```bash
make reset-disk
```

## Running Programs

Most commands can be run by name:

```text
hello
date
uptime
top
cpuz
```

You can also run an ELF explicitly:

```text
runelf usr/bin/hello
runelf hello alpha beta
```

Background jobs use `bg`, `jobs`, `fg`, and `kill`:

```text
bg usr/sbin/tcpecho
jobs
fg 1
```

If a foreground job is running, `Ctrl+Z` returns it to the shell job table.

## Compiling C Inside SmallOS

SmallOS ships TinyCC as `tcc`. Sample source files are installed under
`/usr/share/examples/tinycc`, and the guest build sysroot is installed under
`/usr/include` and `/usr/lib`.

Try this from the SmallOS shell:

```text
cd /var/tmp
tcc -o tccsysroot.elf /usr/share/examples/tinycc/tccsysroot.c
runelf /var/tmp/tccsysroot
tcc -o tccposix.elf /usr/share/examples/tinycc/tccposix.c
runelf /var/tmp/tccposix
```

The older freestanding samples are still useful when testing direct `_start`
programs:

```text
cd /var/tmp
tcc -nostdlib -o tccmini.elf /usr/share/examples/tinycc/tccmini.c
runelf /var/tmp/tccmini
```

Generated programs written under `/var/tmp` remain on the mutable guest disk
until you remove them or run `make reset-disk` on the host.

## Networking Basics

The default QEMU run uses user-mode NAT and DHCP. Check network state with:

```text
ip
ipconfig /all
netinfo
netcheck
pinggw
pingpublic
```

`pinggw` checks the DHCP gateway. `pingpublic` is best-effort because public
ICMP can be blocked by the host, hypervisor, or surrounding network.

To refresh DHCP:

```text
dhcp
```

or:

```text
ip dhcp
```

Temporary manual configuration is also available:

```text
ip addr add 192.168.100.2/24 gateway 192.168.100.1 dns 1.1.1.1
ip route add default via 192.168.100.1
ip dns set 1.1.1.1
```

Network settings are runtime-only and reset on reboot.

## FTP And Web Services

SmallOS starts these services by default:

```text
ftpd    on guest port 2121, passive data port 30000
cserve  on guest port 8080, serving /var/www
```

Under QEMU user networking, inbound connections need host port forwarding. For
FTP:

```bash
make run-headless \
  QEMU_NET_HOSTFWD=',hostfwd=tcp::2121-:2121,hostfwd=tcp::30000-:30000'
```

Connect with passive mode:

```text
host: 127.0.0.1
port: 2121
user: ftp
pass: ftp
```

For the web server:

```bash
make run-headless \
  QEMU_NET_HOSTFWD=',hostfwd=tcp::8080-:8080'
```

Then open:

```text
http://127.0.0.1:8080/
```

You can start extra service instances from the shell:

```text
bg usr/sbin/ftpd --log-file /var/log/ftpd.log
bg usr/sbin/cserve --config /etc/cserve.ini
```

## Demos To Try

Text and system demos:

```text
about
top
meminfo
memmap
cpuz
diskview
```

Graphics and input demos work best with `make run-gtk` or `make run-sdl`:

```text
plasma
mandel
fractint
bmpview /boot/splash.bmp
mousetest
gui
```

`fractint` is the upstream Xfractint 20.04p16 codebase running through a
SmallOS framebuffer adapter. With no arguments it starts a Mandelbrot image in
the built-in `video=F2` mode, using `inside=0` so the Mandelbrot interior is
black:

```text
fractint
fractint type=newton maxiter=80
fractint type=complexnewton maxiter=80
fractint type=julia params=-0.74543,0.11301
```

The port uses Fractint's normal upstream renderer and keyboard/menu flow. The
SmallOS adapter in `src/user/ports/fractint/` supplies a 1024x768 256-color
indexed framebuffer mode and consumes decoded keyboard controls from the public
`term_keys.h` runtime helper. Indexed pixels, palette conversion, and dirty
presentation go through the shared `gfx_indexed` helper; menu text is drawn
through the shared `gfx_text` framebuffer text-cell helper. Generic hosted-C
support is part of the SmallOS user runtime: `src/user/libc/` builds `libc.a`,
`src/user/libm/` builds `libm.a`, and `src/user/posix/` builds syscall-backed
POSIX wrappers in `libposix.a`; those headers and libraries are also installed
in the guest under `/usr/include` and `/usr/lib`. Fractint's common sources use
that public header/runtime surface, and the vendored `third_party/fractint`
tree stays unmodified for SmallOS compatibility. The generated help database is
installed at `/usr/share/xfractint/fractint.hlp`.
Fractint still owns the 256-entry palette; the adapter converts Fractint's VGA
DAC values to the XRGB8888 pixels used by the SmallOS display syscall. Press
`Q` to leave the framebuffer view.

Fractint support files are staged under `/usr/share/xfractint`, including
color maps, parameter sets, formulas, L-system definitions, and IFS
definitions. They are installed both at that search root and in their canonical
upstream subdirectories, so names such as `altern.map` and paths such as
`maps/altern.map` both resolve normally.

The GUI opens a desktop with Files, Shell, System, Config, About, and Quit
icons. Shell windows run real child shells through PTYs. The Config window can
toggle the GUI perf readout; on writable ATA-backed boots the setting is saved
in `/etc/gui.conf`, while USB mass-storage boots are read-only for now.

## Testing A Build

For a quick confidence check from the host:

```bash
make test
```

For broader checks:

```bash
make verify
make verify-network
make verify-display
make verify-full
```

These are mostly for development, but they are useful when you want to know
whether your local image is healthy.

## Common Fixes

If QEMU starts but keyboard input feels awkward, try:

```bash
make run-gtk
```

If files inside the guest look stale or you want a clean disk:

```bash
make reset-disk
make
```

If third-party source directories are missing:

```bash
make deps
```

If FTP connects but directory listings or transfers hang, make sure both ports
are forwarded and your client is in passive mode:

```text
2121   FTP control
30000  FTP passive data
```

## Where To Go Next

Inside SmallOS:

```text
man smallos
man shell
man tcc
man ftpd
man cserve
```

In the repo:

```text
README.md          project overview
docs/              technical subsystem notes
CHANGELOG.md       recent project history
man/               manual pages installed into the guest
```
