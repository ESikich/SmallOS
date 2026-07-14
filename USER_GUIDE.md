# SmallOS User Guide

This guide is for using SmallOS, not for studying how it is built inside. If
you want the kernel, bootloader, filesystem, scheduler, or syscall details, see
the technical notes under `docs/`.

## What SmallOS Is

SmallOS is a small 32-bit x86 operating system that boots into its own shell.
It has a writable filesystem, user programs, a text editor, manual pages,
network tools, BusyBox-backed Unix commands, a tiny C compiler, GNU binutils,
and simple FTP and HTTP services.

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
gcc-multilib
python3
qemu-system-i386
svn
curl
patch
bzip2
unzip
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

That also exports the official Fractint source from
`https://svn.fractint.net/tags/fractint-20-04p17`.

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
busybox --help
/bin/sh -c true
cat /proc/meminfo
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
/proc                virtual process and system status files
/dev                 virtual null, zero, tty, console, and fd nodes
/etc                 passwd, group, services, and system configuration
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

You can also run a program by path:

```text
usr/bin/hello
hello alpha beta
```

Native SmallOS commands stay first. If a bare command is not found in `/bin`,
`/usr/bin`, or `/usr/sbin`, the shell tries `/usr/bin/busybox <command> ...`.
That BusyBox binary is built from unmodified upstream source against the
SmallOS libc/sysroot compatibility layer, making applets such as `grep`, `sed`,
`awk`, `df`, `du`, `free`, `ps`, `tar`, `gzip`, `gunzip`, checksum tools, and
`hexdump` available without replacing native tools. `/bin/sh` is a tiny
launcher for BusyBox `ash`; `/bin/shell` remains the interactive SmallOS shell.

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
tcc -o tccsysroot /usr/share/examples/tinycc/tccsysroot.c
/var/tmp/tccsysroot
tcc -o tccposix /usr/share/examples/tinycc/tccposix.c
/var/tmp/tccposix
```

The older freestanding samples are still useful when testing direct `_start`
programs:

```text
cd /var/tmp
tcc -nostdlib -o tccmini /usr/share/examples/tinycc/tccmini.c
/var/tmp/tccmini
```

Generated programs written under `/var/tmp` remain on the mutable guest disk
until you remove them or run `make reset-disk` on the host.

GNU binutils tools are also available in `/usr/bin` for inspecting and
manipulating ELF files:

```text
as --version
ld --version
readelf -h /usr/bin/hello
objdump -f /usr/bin/hello
nm /usr/bin/hello
size /usr/bin/hello
```

The automated guest suite also runs `usr/libexec/tests/binutilsprobe`, which
creates a tiny assembly file under `/tmp`, assembles it, archives and indexes
it, links a generated ELF, inspects that ELF with the binutils readers, copies
and strips it, and executes the linked, copied, and stripped outputs. That test
is the supported SmallOS-hosted binutils coverage target; it is not intended to
replace GNU binutils' full upstream testsuite.

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

`fractint` is the upstream Xfractint 20.04p17 codebase running through a
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
that public header/runtime surface, and the official SVN export in
`third_party/fractint` stays unmodified for SmallOS compatibility. The generated
help database is installed at `/usr/share/xfractint/fractint.hlp`.
Fractint still owns the 256-entry palette; the adapter converts Fractint's VGA
DAC values to the XRGB8888 pixels used by the SmallOS display syscall. Press
`Q` to leave the framebuffer view.

Fractint support files are staged under `/usr/share/xfractint`, including
color maps, parameter sets, formulas, L-system definitions, and IFS
definitions. They are installed both at that search root and in their canonical
upstream subdirectories, so names such as `altern.map` and paths such as
`maps/altern.map` both resolve normally.

`wolf3d` stages the original id-Software Wolfenstein 3-D source-port work as a
guest-visible command:

```text
wolf3d
```

The upstream source is a clean git submodule at `third_party/wolf3d`. SmallOS
does not ship Wolfenstein 3-D game data; place data files such as `VSWAP.WL6`,
`GAMEMAPS.WL6`, and `VGAGRAPH.WL6` in the host-side `.state/wolf3d/` cache, or
run `make wolf3d-shareware-data` to download and extract the public shareware
v1.4 `.WL1` data set from Wolf3D.net. Normal image builds stage cached files
under `/usr/share/wolf3d`; use `WOLF3D_STAGE_DATA=0` for a lean image without
game data. The command changes into that directory and runs the generated
upstream engine through SmallOS DOS, VGA, input, timer, file, and config
compatibility shims. Current builds reach the sign-on/title/menu flow and the
first gameplay frames with the original data files, and the regression probes
cover the first level-completion intermission path. `CONFIG.` is stored in the
original DOS-width layout, and Wolf-owned display/input/sound state is released
on normal and error exits so the shell gets a clean foreground back.

For manual runtime checks, use a graphical QEMU backend and grab the guest
window before testing mouse input. GTK builds support `Ctrl+Alt+G` to toggle
mouse/keyboard grab; on QEMU for Windows, GTK is usually more predictable than
SDL for this path. Bounded host/guest probes live in
`usr/libexec/tests/wolf3d-srcprobe` and the host-side
`make wolf3d-source-probe` target. The `--level-completed` probe is useful
when changing timer, input, or sound code because it exercises Wolf's
post-level bonus-counting waits.

The GUI opens a desktop with Files, Shell, System, Config, About, and Quit
icons. Shell windows run real child shells through PTYs. The Config window can
toggle the GUI perf readout; on writable ATA-backed boots the setting is saved
in `/etc/gui.conf`, while USB mass-storage boots are read-only for now. Drag a
window's title bar to move it or its bottom-right grip to resize it. When a
text-entry window is not focused, `F`, `T`, `S`, `C`, and `A` open the matching
windows, `X` closes the focused window, and `Q` or Escape exits the desktop.
Double-click a text file in Files, or run `gui <text-path>`, to edit it inside
the desktop. The Editor toolbar provides New, Open, and Save As; Ctrl+N,
Ctrl+O, and Ctrl+Shift+S provide the same workflows from the keyboard. F2 or
Ctrl+S saves, opening Save As automatically for an untitled document. The
shared file picker supports directory navigation, filename entry, Tab focus,
wheel scrolling, and draggable scrollbar thumbs. Navigation, typing, mouse
placement, scrolling, and resizing work normally. Drag across
text or hold Shift while navigating to highlight a selection. Ctrl+A selects
all; Ctrl+C, Ctrl+X, and Ctrl+V copy, cut, and paste through a clipboard shared
by the desktop's editor windows. Closing a modified document asks whether to
save, discard, or cancel, including when New or Open would replace it. The
terminal-oriented `/bin/edit` remains available.

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
