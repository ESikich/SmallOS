# SmallOS

SmallOS is a BIOS-based x86 hobby operating system built with:

* `nasm`
* `i686-elf-gcc`
* `i686-elf-ld`
* `QEMU`

It boots from a raw disk image, switches to 32-bit protected mode, enables paging, loads user programs from a FAT16 partition on disk, and runs a C kernel with a terminal shell, ring-3 ELF program execution, a preemptive round-robin scheduler, voluntary yielding, inter-process exec, and ATA PIO disk access.

---

## Current Features

* Two-stage BIOS bootloader (real mode → protected mode)
* LBA extended disk reads (INT 0x13 AH=0x42) — no CHS track limit
* Kernel-owned GDT with ring-3 user segments and TSS
* x86 paging — identity-mapped first 8 MB
* BSS zeroing in kernel entry before paging is enabled
* IDT with PIT timer (IRQ0), keyboard (IRQ1), and syscalls (INT 0x80)
* Page-fault handler that logs `CR2`, the error code, and user-vs-kernel context; user faults terminate only the offending process
* COM1 serial driver — mirrors all terminal output to QEMU's serial backend for headless testing
* VGA text-mode display and terminal abstraction
* PCI bus scan at boot — groundwork for future NIC support
* e1000 NIC bring-up — PCI discovery, MMIO mapping, and DMA ring setup
* Shell with line editing, history, and command parsing
* Shell input processing decoupled from IRQ1 via a small event queue
* Bump allocator (`kmalloc`) for permanent kernel structures
* Physical memory manager (`pmm`) — bitmap allocator for reclaimable frames
  * Manages `0x200000`–`0x7FFFFF` (6 MB, 1536 frames)
  * All process frames reclaimed on exit — no leak after `runelf`
* ELF loader — validates, loads segments, zeroes BSS, launches in ring 3
* `process_t` abstraction — per-process struct (PD, kernel stack, scheduler state, argv storage, name); fully PMM-allocated and reclaimed on exit
* Per-process page directories — address space isolation; PD itself freed on exit
* Ring 3 user mode — hardware-enforced privilege separation
* Per-process kernel stacks — dedicated PMM frame per process; TSS ESP0 set from it; freed on exit
* Syscall layer via `int 0x80` (DPL=3 gate): `SYS_WRITE`, `SYS_EXIT`, `SYS_GET_TICKS`, `SYS_PUTC`, `SYS_READ`, `SYS_YIELD`, `SYS_SLEEP`, `SYS_EXEC`, `SYS_WRITEFILE`, `SYS_WRITEFILE_PATH`, plus file-handle helpers (`SYS_OPEN_WRITE`, `SYS_WRITEFD`, `SYS_LSEEK`, `SYS_UNLINK`, `SYS_RENAME`, `SYS_STAT`)
* Socket ABI via `int 0x80`: `SYS_SOCKET`, `SYS_BIND`, `SYS_LISTEN`, `SYS_ACCEPT`, `SYS_CONNECT`, `SYS_SEND`, `SYS_RECV`, and `SYS_POLL`
* `sys_exit()` is scheduler-owned: it switches to the kernel page directory, marks the current task `PROCESS_STATE_ZOMBIE`, and switches to the next runnable task
* Shell now runs as an explicit kernel task scheduled by `scheduler.c`
* **Preemptive round-robin scheduler** — timer IRQ (100 Hz) context-switches between kernel tasks; 10-tick (100 ms) quantum
* **`SYS_YIELD`** — voluntary preemption; process surrenders its remaining quantum immediately
**`SYS_EXEC`** — user process asynchronously spawns a named child ELF and returns `0` on success / `-1` on failure
* **`SYS_WRITEFILE`** — user process creates or overwrites a root-directory FAT16 file in one shot
* **`SYS_WRITEFILE_PATH`** — user process creates or overwrites a FAT16 file at any nested path
* **ATA PIO driver** — polls the primary IDE channel (`0x1F0`) to read 512-byte sectors from disk in 32-bit protected mode; no DMA or IRQ required
* **FAT16 partition** — 16 MB FAT16 volume appended to the disk image containing all user ELFs; built by `tools/mkfat16.c` with no external dependencies; readable via ATA PIO with nested directory paths, writable at runtime for root-directory files and nested paths
* **TCP bring-up task** — minimal kernel-side TCP listener/echo path for end-to-end networking validation before the socket ABI is exposed to user space
* **Guest TCP apps** — `apps/services/tcpecho.elf` proves the socket path end to end, and `apps/services/ftpd.elf` runs the vendored FTP session logic as a normal user-space ELF

---

## Project Structure

```text
.
├── docs/           documentation
├── src/
│   ├── boot/       boot.asm, loader2.asm, kernel_entry.asm
│   ├── kernel/     kernel.c, gdt, idt, paging, memory, pmm, process,
│   │               scheduler, sched_switch.asm, syscall, timer, system, setjmp
│   ├── drivers/    keyboard, screen, terminal, ata, fat16
│   ├── shell/      shell, line_editor, parse, commands
│   ├── exec/       elf_loader
│   └── user/       hello.c, ticks.c, args.c, readline.c, exec_test.c,
│                   compiler_demo.c, fault.c, sleep_test.c, user_lib.h, user_syscall.h
├── tools/
│   ├── mkfat16.c
│   └── mkimage.c
├── build/
├── Makefile
├── linker.ld
```

The seeded FAT16 image keeps shipped programs under `apps/demo/` and
`apps/tests/`, with TinyCC under `tools/` and its sample sources at the
image root for the shell demo that moves them into `samples/`.

---

## Build & Run

```bash
make clean && make
```

**Interactive (VGA in terminal):**
```bash
make run          # requires a terminal that supports curses
```

**Headless (background, serial log):**
```bash
make run-headless           # starts QEMU as a daemon
tail -f /tmp/smallos-serial.log   # read all kernel output
```

**TAP / bridge networking:**
```bash
make run-tap QEMU_NET_IFACE=tap0
```

`run-tap` tells QEMU to attach the e1000 NIC to a host TAP device instead of
using the built-in user-network NAT. The TAP interface must already exist on
the host; if you want the guest to reach your LAN or the internet, bridge that
TAP device into your host network stack first.

One common Linux setup is:
```bash
sudo ip tuntap add dev tap0 mode tap user "$USER"
sudo ip link set tap0 up
sudo ip addr add 192.168.100.1/24 dev tap0
```

To put the guest on a real bridged network, connect `tap0` to an existing host
bridge or routing setup that already has upstream access.

On Windows, QEMU's TAP mode depends on a separate TAP driver; the standard
QEMU for Windows packages do not include it. If you are staying on Windows and
do not want to install extra networking drivers, keep using the default
`make run` / `make test` user-network NAT path.

**Windows / PowerShell debug run:**
```powershell
qemu-system-i386 -drive format=raw,file=os-image.bin -m 32 -serial stdio -d int,cpu_reset,guest_errors -D qemu.log -display gtk 2>&1 | Tee-Object -FilePath qemu-console.log
```

`run-headless` daemonizes QEMU, writes a PID to `/tmp/smallos.pid`, and
mirrors all terminal output to `/tmp/smallos-serial.log` via the COM1
serial driver.  Use the QEMU monitor socket at
`/tmp/smallos-monitor.sock` to send keystrokes (`sendkey`) or take
screenshots (`screendump`).

The mutable FAT16 disk state now lives in `.state/fat16.img`, so normal
rebuilds keep your files.  Use `make reset-disk` if you want to restore
the seeded filesystem from the latest build.

For the TCP smoke path, launch the guest with host forwarding for the
service port you want to exercise. For example:

```bash
qemu-system-i386 \
  -drive format=raw,file=build/img/os-image.bin \
  -boot c -m 32 \
  -serial file:/tmp/smallos-serial.log \
  -nic user,model=e1000,mac=52:54:00:12:34:56,hostfwd=tcp::2462-:2323,hostfwd=tcp::2121-:2121 \
  -display none \
  -monitor unix:/tmp/smallos-monitor.sock,server,nowait \
  -daemonize -pidfile /tmp/smallos.pid
```

Then run `runelf_nowait apps/services/tcpecho` or `runelf_nowait
apps/services/ftpd` in the guest shell and connect from the host to
`127.0.0.1:2462` or `127.0.0.1:2121` respectively.

`make test` boots the image headlessly, runs the shell `selftest`
command, feeds the interactive `readline` prompt, and checks every
shipped ELF in one pass.  The built-in shell command expectations live
under `tests/shell/` and the ELF expectations live under `tests/elfs/`.

`make smoke` runs the dedicated reboot and halt smoke checks.  Use
`make smoke-reboot` or `make smoke-halt` if you want to exercise one
command at a time.

The PowerShell command keeps a GTK window visible while capturing guest output in the console and saving `qemu.log` plus `qemu-console.log` for later debugging.

Use `-drive format=raw` (hard disk mode). Do not use `-fda` (floppy).

---

## Commands

```text
help
clear
echo [args...]
about
halt
reboot              reboot the machine
uptime
meminfo
ataread <lba>        dump first 32 bytes of a disk sector (ATA PIO)

fsls [path]        list a FAT16 directory (root by default)
ls [pattern]       list a FAT16 directory, with `*` and `?` globbing
fsread <name>      dump first 16 bytes of a FAT16 file
cat <path>          print a FAT16 file
cd <path>           change the shell working directory
pwd                 print the shell working directory
netinfo             show PCI NIC status
netsend             queue a test Ethernet frame
netrecv             poll and dump one Ethernet frame
arpgw               resolve the QEMU gateway via ARP
ping <ip>           ping an IPv4 address
pinggw              ping the QEMU gateway
pingpublic          ping 1.1.1.1 to probe internet reachability
netcheck            check gateway and public connectivity
mkdir <path>       create a FAT16 directory
rmdir <path>       remove an empty FAT16 directory
rm <path>          remove a FAT16 file
touch <path>       create or truncate a FAT16 file
cp <src> <dst>     copy a FAT16 file
mv <src> <dst>     move or rename a FAT16 entry
runelf <name> [args] load and run an ELF from the FAT16 partition
runelf_nowait <name> [args] enqueue an ELF and return immediately
selftest            run all shipped ELF self-tests
shelltest           run built-in shell command tests
```

Seeded FAT16 layout:
- `echo`, `about`, `uptime`, `halt`, `reboot` in the image root
- `apps/demo/hello` - print argc/argv and tick count
- `apps/tests/ticks` - print the current tick count
- `apps/tests/args` - print argc and argv
- `apps/tests/runelf_test` - verify ELF loading, syscalls, and stack setup
- `apps/tests/readline` - interactive SYS_READ demo
- `apps/tests/exec_test` - exercise SYS_EXEC semantics
- `apps/tests/fileread` - exercise process-owned file handles via SYS_OPEN / SYS_FREAD / SYS_CLOSE
- `apps/tests/compiler_demo` - exercise SYS_WRITEFILE, SYS_WRITEFILE_PATH, and readback
- `apps/tests/heapprobe` - exercise malloc/free/realloc/calloc
- `apps/tests/statprobe` - exercise SYS_STAT and path probing
- `apps/tests/fileprobe` - exercise small file wrapper helpers
- `apps/tests/sleep_test` - exercise SYS_SLEEP semantics
- `apps/tests/ptrguard` - exercise syscall pointer validation
- `apps/tests/preempt_test` - prove timer-driven preemption between runnable tasks
- `apps/tests/fault` - fault probe (ud/gp/de/br/pf)
- `tools/tcc.elf` in `tools/`
- `samples/tccmath.c`, `samples/tccagg.c`, `samples/tcctree.c`, `samples/tccmini.c` at the image root for the guest compiler demo

`help` renders the built-in shell command list from the command table and the shipped-program list from the program table, with the same short descriptions.

`pinggw` only proves the QEMU NAT gateway works. `pingpublic` routes the echo
request through that gateway to a public IP, and `netcheck` prints each step so
you can see whether the failure is local to the gateway or beyond it.

---

## Architecture Overview

```text
BIOS
 → boot.asm              load loader2 (CHS, 4 sectors) to `0x40000`
 → loader2.asm           load kernel (LBA) → protected mode
 → kernel_entry.asm      zero BSS → kernel_main()

kernel_main()
 → terminal_init()       VGA text mode / terminal output
 → gdt_init()            GDT: null, k-code, k-data, u-code, u-data, TSS
 → paging_init()         enable paging, identity-map 8 MB
 → memory_init()         bump allocator at 0x100000
 → pmm_init()            bitmap allocator at 0x200000
 → kernel_selfcheck()    verify TSS, stack, heap, and PMM baselines
 → keyboard/timer/idt    drivers and interrupt table
 → sched_init()          initialise runnable task table
 → ata_init()            initialise ATA primary channel
 → pci_init()            scan PCI devices and log discovered network controllers
 → fat16_init()          read BPB, validate FAT16 volume
 → create shell task     explicit kernel task with its own stack
 → process_start_reaper() create and enqueue the zombie reaper task
 → sti
 → sched_start(shell)    switch from boot stack into shell task

runelf apps/demo/hello
 → process_create()      allocate process_t from PMM
 → process_pd_create()   fresh page directory (PMM), kernel entries shared
 → map ELF + stack       pmm_alloc_frame() per page
 → alloc kernel stack    per-process ring-0 stack for syscalls / interrupts
 → seed sched context    proc->sched_esp returns into elf_user_task_bootstrap()
 → sched_enqueue(proc)   child becomes a real runnable task
 → process_wait(proc)    shell blocks in a safe wait loop
 → scheduler enters child via elf_user_task_bootstrap()
 → elf_enter_ring3()     iret into ring 3 with IF set in EFLAGS
 → sys_exit()            switch to kernel PD, mark ZOMBIE, switch away
 → shell wakes, process_destroy() reclaims PMM frames
```

---

## Disk Image Layout

```text
LBA 0           boot.bin              (512 bytes)
LBA 1–4         loader2.bin           (currently 2048 bytes)
LBA 5+          padded kernel region  (sector-aligned)
LBA 5+ks
               FAT16 partition        (16 MB volume inside the image)
```

`MBR_PARTITION_TABLE_OFFSET` and `MBR_PARTITION_ENTRY_SIZE` are declared in `src/boot/boot.asm`. Stage 2 reads the kernel range from partition entry 0, and the kernel discovers the FAT16 partition from entry 1.

During image assembly, the Makefile reads those declarations and passes them to `mkimage`, which computes:

```text
kernel_lba = 1 + loader2_sectors
FAT16_LBA = kernel_lba + kernel_sectors
```

The resulting kernel and FAT16 spans are written into the MBR partition table in sector 0 after image assembly.

`make verify` runs the full preflight: boot-layout check, image-layout check, guest `test`, and `smoke`. Use `make boot-layout-check` or `make image-layout-check` when you want to isolate a specific layer.

---

## Build Tools

```text
mkfat16   builds the FAT16 volume containing user ELFs
mkimage   assembles the final bootable disk image
```

---

## Physical Memory Layout

```text
0x00007C00   bootloader stage 1
0x00040000   loader2 stage 2
0x00001000   kernel image
0x00006000   kernel .bss start (page tables + PMM bitmap)
~0x0000A000  kernel .bss end (approx)
0x001FF000   kernel stack top (boot / shell context)
0x00100000   bump allocator — permanent kernel structures
0x00200000   PMM — reclaimable frames
               process_t structs, process PDs, ELF frames,
               user stack frames, all process-private PTs, kernel stack frames
0x00800000   PMM ceiling (= identity-map limit)
0x00400000   USER_CODE_BASE (per-process ELF mapping)
0xBFFFF000   user stack (per-process mapping)
```

---

## Scheduler

Round-robin, preemptive, timer-driven at 100 Hz with a 10-tick (100 ms) quantum.

The scheduler uses a fixed-capacity table:

    s_table[SCHED_MAX_PROCS]

This table is a **plain dynamic array**, not a structured layout:

- There is **no special slot 0**
- There is **no sentinel process_t**
- Entries are appended by `sched_enqueue()` and compacted by `sched_dequeue()`
- `sched_start()` searches for the requested process instead of assuming a fixed position

The `pd == 0` rule still exists, but it is **not a table-layout concept**. It is interpreted at runtime:

- `pd == 0` → use kernel page directory
- otherwise → use process page directory

Today, the scheduler owns both kernel tasks such as the shell and ELF user programs. `runelf` waits for the child with `process_wait()`, while `runelf_nowait` returns immediately after enqueue.

---

## Context Switch Path

```text
timer interrupt
  ↓
irq0_stub
  ↓
irq0_handler_main(esp)
  ↓
sched_tick(esp - 8)
  ↓
sched_switch(&cur->sched_esp, nxt->sched_esp, next_cr3, next_esp0)
  ↓
tss_set_kernel_stack(next_esp0)
  ↓
CR3 switch
  ↓
ESP switch
  ↓
ret → resumed context
```

This path no longer relies on a cached pointer into the packed TSS structure.

---

## Notes

* Launch as a hard disk, not a floppy
* User programs are linked at `0x400000`
* `fat16_load()` uses a static load buffer, so repeated `runelf` calls do not grow the heap
* The scheduler switch path updates TSS.ESP0 through `tss_set_kernel_stack()`, not by caching a pointer into the TSS

---

## Current Direction

The scheduler-owned execution model is complete:

* the **shell** is a scheduler-owned kernel task
* **ELF user programs** are scheduler-owned tasks entered through `elf_user_task_bootstrap()`
* `runelf` blocks by waiting for `PROCESS_STATE_ZOMBIE` via `process_wait()`
* `runelf_nowait` and `SYS_EXEC` children are automatically reaped by the **reaper kernel task** (`sched_reap_zombies()`) within one scheduler quantum of exit — no frame leaks
