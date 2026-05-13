# Build/Boot Layout Ownership

Boot-image layout facts are owned by the files that define them, not by the Makefile:

* `src/boot/boot.asm` owns `BOOT_SECTOR_SIZE`
* `src/boot/boot.asm` owns `LOADER2_SECTORS`
* `src/boot/boot.asm` owns `MBR_PARTITION_TABLE_OFFSET`
* `src/boot/boot.asm` owns `MBR_PARTITION_ENTRY_SIZE`
* `src/boot/loader2.asm` owns `LOADER2_SIZE_BYTES`
* `Makefile` owns the generated stage-2 stack top
* `tools/mkext2.c` owns `TOTAL_SIZE_MB` / `TOTAL_BLOCKS`

The Makefile consumes these declarations while building `os-image.bin`, and passes them into `mkimage` for final image assembly.

---

# Architecture Overview

This document describes how SmallOS boots, initializes, and executes programs.
For the ring-3 C runtime contract used by user ELFs, see
[`docs/user-runtime.md`](user-runtime.md).

---

# Boot Flow

```text
BIOS
  ↓
boot.asm (stage 1, 16-bit)
  ↓
loader2.asm (stage 2, 16-bit → 32-bit)
  reads boot-sector metadata, derives kernel LBA, loads kernel → 0x1000
  applies display policy: VBE framebuffer in auto mode, BIOS/VGA text in forced VGA mode
  ↓
kernel_entry.asm (32-bit)
  zeros BSS
  ↓
kernel_main()
  terminal_init()   ← VGA fallback backend + serial output
  gdt_init()        ← null, k-code, k-data, u-code, u-data, TSS + ltr
  paging_init()     ← identity-maps first 8 MB, enables CR0.PG
  memory_init()     ← bump allocator after high kernel BSS
  pmm_init()        ← E820-filtered bitmap allocator at 0x200000–0x7FFFFFF
  boot diagnostics  ← splash PASS/WARN/FAIL checks for startup invariants
  fb_console_init() ← switch to framebuffer terminal when VBE boot info is valid
  keyboard/mouse/timer/idt
  #PF handler      ← logs CR2 / error code, kills user faults, panics on kernel faults
  sched_init()      ← initialise runnable task table
  ata_init()        ← software reset ATA primary channel, verify ready
  pci_init()        ← scan PCI config space and log network controllers
  e1000_init()      ← bind the Intel 82540EM NIC and set up DMA rings
  tcp_init()        ← start TCP/network service task
  ext2_init()      ← read MBR entry 1, validate ext2 superblock and geometry
  create shell task ← explicit kernel task with dedicated stack
  process_start_reaper() ← create and enqueue zombie reaper task
  sti
  sched_start()     ← switch from boot stack into shell task
```

---

## Stage 1 – boot.asm

* Loaded by BIOS at `0x7C00`
* Loads stage 2 via CHS `INT 0x13 AH=0x02` (`LOADER2_SECTORS`, fits within one track) to `0x40000`
* Must be exactly **512 bytes**, ending with `dw 0xAA55`

---

## Stage 2 – loader2.asm

Runs in **real mode**, then switches to **protected mode**.

* Checks `INT 0x13 AH=0x41` for LBA extension support — halts if not available
* Reads partition entry 0 from the MBR partition table, derives the kernel LBA and size from that entry, and loads the kernel to `0x1000` via `INT 0x13 AH=0x42`
* Sets up a generated temporary stack (`SP=0xFF00`, physical top `0x4FF00`), installs a temporary GDT, enables protected mode, and uses a 32-bit far jump into `init_pm`
* In protected mode, switches to `0x1FF000` as the boot/kernel stack top and jumps to kernel entry at `0x1000`

One value injected by Makefile at build time: the generated stack-top constants.

Loader2 GDT is temporary — kernel installs its own immediately.

---

## Kernel Entry – kernel_entry.asm

```asm
extern bss_start
extern bss_end

_start:
    mov edi, bss_start
    mov ecx, bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb            ; zero all BSS including page tables and PMM bitmap

    call kernel_main
```

BSS zeroing is mandatory. The three paging structures and the PMM bitmap live
in `.bss`, which the linker places at `0x100000` as a `NOLOAD` section. That
keeps the flat `kernel.bin` compact and prevents BSS zeroing from overwriting
loader-owned low memory such as boot info at `0x90000`. Without zeroing,
`paging_init()` can triple-fault and the PMM bitmap would falsely show all
frames as used.

---

# Kernel Initialization

Inside `kernel_main()`:

1. `terminal_init()` — VGA fallback backend and serial output
2. `gdt_init()` — install GDT with ring-3 segments and TSS; load task register with `ltr`
3. `paging_init()` — enable paging, identity-map 8 MB
4. `memory_init(PAGE_ALIGN(&bss_end))` — bump allocator starts after high kernel BSS
5. `pmm_init()` — bitmap allocator covers 0x200000–0x7FFFFFF; E820 usable ranges are freed, then boot/runtime reservations are marked used again
6. `kernel_selfcheck()` — report splash checks for TSS selector, boot stack, heap base after BSS, and PMM baseline
7. `fb_console_init()` — map and select the framebuffer backend when VBE boot info is valid
8. `keyboard_init()`, `mouse_init()`, `timer_init(SMALLOS_TIMER_HZ)`, `idt_init()` — input/timer drivers and interrupt table
9. `sched_init()` — initialise the scheduler data structures
10. `ata_init()` — software reset ATA primary channel (`0x1F0`), poll until ready
11. `pci_init()` — scan PCI config space and log discovered network controllers
12. `e1000_init()` — bind the Intel 82540EM NIC and set up DMA rings
13. `tcp_init()` — create and enqueue the TCP/network service kernel task
14. `ext2_init()` — read ATA sector 0, extract the ext2 start LBA from partition entry 1 in the MBR partition table, then read and validate the ext2 superblock at that runtime-discovered location
15. `process_create_kernel_task("shell", shell_task_main)` — create the shell as an explicit kernel task
16. `sched_enqueue(shell_proc)` — make the shell runnable
17. `process_start_reaper()` — create and enqueue the zombie reaper kernel task
18. `sti` — enable interrupts
19. `sched_start(shell_proc)` — switch from the boot stack into the shell task

`sched_init()` must still be called before `sti`, and `sched_start()` must happen only after the first runnable task has been created.

---

# GDT Layout

```text
Index 0   selector 0x00   null
Index 1   selector 0x08   kernel code   DPL=0   access=0x9A  gran=0xCF
Index 2   selector 0x10   kernel data   DPL=0   access=0x92  gran=0xCF
Index 3   selector 0x1B   user code     DPL=3   access=0xFA  gran=0xCF
Index 4   selector 0x23   user data     DPL=3   access=0xF2  gran=0xCF
Index 5   selector 0x28   TSS           DPL=0   32-bit available TSS
```

Selector values for ring-3 entries include RPL=3 in the low two bits (`0x18|3=0x1B`, `0x20|3=0x23`).

---

# Process Abstraction

Each `runelf` invocation still creates a `process_t` struct allocated from a single PMM frame. The same structure is now also used for kernel tasks such as the shell:

```c
typedef struct {
    u32*            pd;                     /* PMM-allocated page directory        */
    u32             kernel_stack_frame;     /* PMM frame for ring-0 syscall stack  */
    unsigned int    sched_esp;              /* kernel ESP saved on preemption      */
    process_state_t state;                  /* UNUSED / RUNNING / WAITING / ZOMBIE / EXITED */
    void          (*kernel_entry)(void);    /* kernel task entry point             */
    unsigned int    user_entry;             /* first ring-3 EIP for ELF tasks      */
    int             user_argc;              /* saved argc for bootstrap            */
    char*           user_argv[17];          /* saved argv pointers plus NULL       */
    char            user_arg_data[256];     /* argv string storage (kernel-side)   */
    char            name[32];               /* null-terminated process name        */
    fd_entry_t*     fds;                    /* PMM-backed handle table             */
    unsigned int    fd_capacity;            /* allocated fd slots                  */
    unsigned int    fd_limit;               /* per-process fd limit                */
} process_t;
```

For runnable tasks, `sched_esp` is the saved kernel resume stack pointer used by the scheduler. Kernel tasks and ELF tasks can both have a valid seeded `sched_esp` before their first timer-driven switch.

`process_set_args()` copies argv strings into `user_arg_data`, populates
`user_argv`, and guarantees `user_argv[user_argc] == NULL`. This process-owned
storage is independent of the shell input buffer or syscall caller memory and
remains valid until the process exits.

The shell task itself is just another kernel task with a small 4 KB kernel stack,
so the scripted `shelltest` / `selftest` command tables are kept in static
storage rather than on the stack. That avoids trampling `process_t` state during
the longest regression paths.

The handle table is generic rather than file-specific now. `process_create()`
pre-opens fd `0`, `1`, and `2` as console handles for stdin/stdout/stderr.
User-opened files and sockets start at fd `3`. The table starts with 16 slots,
grows from PMM-backed contiguous frames on demand, and defaults to a 128-fd
process limit with a 256-fd hard cap.

Each handle slot carries its own kind and ops table:

```text
read / write / seek / poll / flush / close
```

`process.c` owns descriptor allocation, lifetime, and generic dispatch. The
resource backends own the behavior behind each handle: ext2-backed file
handles are initialized by `vfs_file_init()` and implemented in `vfs.c`,
socket handles point at `socket_t` objects implemented in `socket.c`, and
socket blocking readiness is tracked by socket-owned wait queues. The TCP
driver owns passive listeners plus outbound active-open streams in a
PMM-backed global 4-tuple connection table, with lazy 4 KiB PMM-backed RX
rings and lazy 16 KiB PMM-backed TX rings for streams; TX payloads remain
queued until ACKed, writable readiness reflects remaining TX capacity, and
global RX/TX caps bound ring allocation. Basic socket `shutdown()` half-close
state is split between the
socket object and TCP stream so `SHUT_RD` reports local EOF and `SHUT_WR`
drains queued TX before sending FIN, with FIN retransmission and late cleanup
on the passive close path. Console handles own terminal writes and
keyboard-buffer reads.
`syscall.c` therefore stays focused on user-pointer validation and dispatch
instead of knowing the internals of each resource type.

The user-space view of this fd table, including POSIX wrappers, stdio stream
state, cwd-relative path handling, and directory traversal, is documented in
[`docs/user-runtime.md`](user-runtime.md).

---

# Scheduler

A round-robin preemptive scheduler runs on every timer tick. The hardware rate is `SMALLOS_TIMER_HZ`, and the scheduler quantum is expressed as `SCHED_QUANTUM_MS` rather than a hardcoded tick count.

## Process table

Fixed array of up to 8 `process_t*` entries stored in `s_table[SCHED_MAX_PROCS]`.

There is **no reserved slot 0** and no static sentinel `process_t`. The scheduler
treats the table as a simple dynamic array:

- `sched_enqueue()` appends a process at index `s_count`
- `sched_dequeue()` removes a process and compacts the array
- `sched_start()` scans the table to locate the requested starting process

All entries are treated uniformly — no index has special meaning.

The `pd == 0` convention still exists, but it is independent of table position:

- If `proc->pd == 0`, the kernel page directory is used
- Otherwise, the process page directory is used

This is handled dynamically when selecting CR3 (see `sched_proc_cr3()` in `scheduler.c`).

## Context switch path

```text
timer fires (IRQ0)
  ↓
irq0_stub — pusha + segment pushes, then push esp, call irq0_handler_main
  ↓
irq0_handler_main(esp)
  timer_handle_irq()
  EOI sent here (before sched_tick — see ordering note below)
  sched_tick(esp)
    ↓
    [if quantum not expired] return
    ↓
    save esp → s_table[current]->sched_esp
    pick next runnable entry (skip if sched_esp == 0 or state != RUNNING)
    sched_switch(&cur->sched_esp, nxt->sched_esp, next_cr3, next_esp0)
      load all args into registers
      *save_esp = current esp
      tss_set_kernel_stack(next_esp0)
      cr3       = next_cr3
      esp       = next_esp
      ret  ← resumes incoming context here
    ↓
    [outgoing context resumes here when switched back to]
  ↓
irq0_handler_main returns
  ↓
irq0_stub — add esp 4, pop segments, popa, iretd → ring 3 resumes
```

**EOI ordering**: EOI is sent before `sched_tick`. If `sched_switch` lands on a different context and the outgoing `irq0_handler_main` never returns through the normal path, the PIC is already unmasked and future timer ticks fire correctly on the new context. Sending EOI after `sched_tick` would permanently mask IRQ0 for the incoming context.

## SYS_YIELD integration

`sched_yield_now(esp)` is called from `sys_yield_impl` inside the syscall handler. It bypasses the quantum counter, resets `s_tick_count`, and calls the same `sched_do_switch(esp)` core used by `sched_tick`. The `esp` argument is `(unsigned int)regs` — a pointer to the `isr128_stub` register frame, which is structurally identical to an `irq0_stub` frame (same push order). `sched_switch` therefore resumes the yielding process via `iretd` exactly as it would a timer-preempted context.

## Lifecycle

```text
runelf (foreground):
  process_create()
  allocate proc->kernel_stack_frame
  seed proc->sched_esp         → first entry via elf_user_task_bootstrap()
  sched_enqueue(proc)
  process_wait(proc)           → shell waits until child is ZOMBIE
  process_destroy(proc)        → called from shell's safe stack on wakeup

runelf_nowait (background):
  process_create()
  allocate proc->kernel_stack_frame
  seed proc->sched_esp         → first entry via elf_user_task_bootstrap()
  sched_enqueue(proc)
  return immediately           → no explicit waiter

SYS_EXEC (parent-waitable spawn):
  process_create()
  allocate proc->kernel_stack_frame
  seed proc->sched_esp         → first entry via elf_user_task_bootstrap()
  sched_enqueue(proc)
  process_claim_for_wait(proc) → parent owns child until waitpid() or parent exit
  return child pid

bg / runelf_bg (reattachable background):
  process_create()
  allocate proc->kernel_stack_frame
  seed proc->sched_esp         → first entry via elf_user_task_bootstrap()
  sched_enqueue(proc)
  process_claim_for_wait(proc) → reaper skips it
  shell job table stores proc  → jobs / fg / kill own cleanup

Ctrl+Z during fg:
  process_key_consumer()
  record detach request
  restore shell keyboard consumer
  process_wait_detachable() returns without destroying the job

sys_exit:
  paging_switch(kernel_pd)
  sched_exit_current((unsigned int)regs)
  mark task ZOMBIE
  switch to next runnable task
  [foreground] process_destroy() called by process_wait() in shell
  [background] process_destroy() called by process-registry reaper
  [SYS_EXEC] process_destroy() called by waitpid(), or by reaper after parent exit
  [shell job] process_destroy() called by fg or kill

reaper task (permanent kernel task):
  loop:
    sched_reap_zombies()       → destroy unclaimed registry ZOMBIE processes
    sti; hlt                   → sleep until next timer tick
```

---

# Interrupt Architecture

## IDT entries

```text
8    → ISR8 (double fault — VGA marker '8' white-on-red at row 1 col 12 + halt)
32   → IRQ0 (timer, DPL=0)
33   → IRQ1 (keyboard, DPL=0)
44   → IRQ12 (PS/2 mouse, DPL=0)
128  → syscall int 0x80 (DPL=3 — callable from ring 3)
```

## IRQ0 flow (with scheduler)

```text
timer fires
  ↓
irq0_stub: pusha, push segments, push esp, call irq0_handler_main(esp)
  ↓
irq0_handler_main:
  timer_handle_irq()
  EOI (outb 0x20, 0x20)        ← before sched_tick
  sched_tick(esp)
    [may call sched_switch and not return to this invocation]
  ↓
irq0_stub: add esp 4, pop segments, popa, iretd
```

## IRQ1 flow

```text
irq1_stub: pusha, push segments, push esp
  ↓
irq1_handler_main:
  EOI (outb 0x20, 0x20)        ← sent before handler (consistent with IRQ0 pattern)
  keyboard_handle_irq()
  ↓
irq1_stub: pop segments, popa, iretd
```

## IRQ12 flow

```text
irq12_stub: pusha, push segments, push esp
  ↓
irq12_handler_main:
  EOI slave PIC (outb 0xA0, 0x20)
  EOI master PIC (outb 0x20, 0x20)
  mouse_handle_irq()
  ↓
irq12_stub: pop segments, popa, iretd
```

## Syscall flow (ring 3 → ring 0 → ring 3)

```text
int 0x80 (CPL=3)
  ↓
CPU: load SS0/ESP0 from TSS — switch to per-process kernel stack
CPU: push SS, ESP, EFLAGS, CS, EIP
  ↓
isr128_stub: pusha, push segments, push esp, call syscall_handler_main(regs)
  ↓
regs->eax = result
  ↓
pop segments, popa, iretd → ring 3 resumes
```

The `isr128_stub` frame (pusha + 4 segment pushes + esp) is structurally identical to the `irq0_stub` frame. This means `regs` (the pointer passed to `syscall_handler_main`) can be passed directly to `sched_yield_now()` as a valid scheduler resume ESP.

Normal syscalls return through this saved interrupt frame. `SYS_EXIT` is the exception: it switches to the kernel page directory, marks the current task `PROCESS_STATE_ZOMBIE` through `sched_exit_current()`, and switches away instead of unwinding through the original `iretd` path.

`SYS_READ` parks the calling process in `PROCESS_STATE_WAITING` and halts with `hlt` until a keypress arrives. It does **not** call `sched_yield_now()` — the process stays on the kernel stack inside `sys_read_impl` and is resumed there by the scheduler after `process_key_consumer()` marks it `RUNNING` again.

---

# Terminal + Shell

## Terminal

`terminal.c` owns terminal semantics and dispatches through an active backend. It provides `terminal_putc`, `terminal_puts`, `terminal_put_uint`, `terminal_put_hex`, cursor control, scrolling behavior, ANSI cursor/clear handling, and runtime `terminal_rows()` / `terminal_cols()` queries. Serial mirrors all terminal bytes and remains the headless test truth.

The default backend is VGA text mode (`0xB8000`) for early/fallback/debug output. When `DISPLAY_BACKEND=auto` and loader2 provides valid VBE boot info, `fb_console.c` maps the linear framebuffer at `0xD0000000` and switches to a white-on-black 8x16-font framebuffer backend. At 1024x768 this exposes a 128x48 terminal while keeping VGA text available for panic/double-fault markers. With `DISPLAY_BACKEND=vga`, loader2 stays in BIOS/VGA text mode, leaves framebuffer boot info invalid, still collects E820, and the kernel leaves the VGA backend active.

User graphics programs sit above the framebuffer display syscalls. The shared
helper in `src/user/gfx.c` queries display geometry, requires XRGB8888/32 bpp,
acquires exclusive graphics mode, allocates a full-screen user backbuffer, and
presents that buffer with one `SYS_DISPLAY_BLIT`. `bmpview` uses this path for
scaled/centered BMP presentation, `bin/diskview` uses it for an ext2 used/free
allocation map, and `usr/bin/plasma` uses it as a simple animated graphics
smoke demo. `usr/bin/mandel` uses the same helper for an interactive
Mandelbrot view and polls `SYS_MOUSE_READ` for cursor deltas.

## Shell

```text
keyboard IRQ → keyboard_handle_irq()
  ↓
  decode scancode → key_event_t
  ↓
  call registered consumer (keyboard_consumer_fn)
  ↓
  [shell consumer]              [process consumer]
  shell_key_consumer()          process_key_consumer()
  enqueue shell_event_t         ASCII → keyboard_buf_push_char()
                                Ctrl+C → signal/terminate foreground group
  ↓                             ↓
  shell_task_main()             SYS_READ drains kb_buf
  drains queue via shell_poll()
  ↓
  line_editor (insert, delete, cursor, history)
  ↓
  [Enter] → parse_command() → commands_execute()
  ↓
  kernel command dispatch or bin/<name>.elf fallback
```

The active consumer is managed by `keyboard_set_consumer()`:
- `shell_init()` registers `shell_key_consumer` at boot
- `process_set_foreground(proc)` clears `kb_buf` (discarding any stale input, e.g. the Enter that launched `runelf`), records the foreground process group, then registers `process_key_consumer` when a user process takes the foreground
- `process_key_consumer` pushes ASCII into `kb_buf`; after each push it checks `keyboard_get_waiting_process()` and, if a process is parked in `PROCESS_STATE_WAITING`, sets it back to `PROCESS_STATE_RUNNING` and clears the waiter slot so the scheduler picks it up
- Ctrl+C is handled by `process_key_consumer` as a terminal interrupt for the foreground process group. Matching signalfds receive `SIGINT`; otherwise the group gets exit status `130`, pending console/socket waits are cleared, and any actively running member is switched away from the IRQ1 frame.
- `process_set_foreground(0)` calls `shell_register_consumer()` to restore the shell consumer on exit

The keyboard driver makes no routing decisions. It decodes scancodes and calls whoever is registered.

Mouse input is intentionally lower-level today. `mouse.c` initializes the PS/2
auxiliary port, decodes 3-byte relative-motion packets on IRQ12, and stores
accumulated `dx`/`dy` plus button bits. User programs call `SYS_MOUSE_READ` to
copy and clear the accumulated movement. There is no unified keyboard/mouse
event queue yet.

---

# User Programs

Entry point convention:

```c
void _start(int argc, char** argv)
```

That convention remains the low-level kernel launch ABI. Hosted-style user programs can
instead link `src/user/user_crt0.c`, define `main(int argc, char** argv)`, and
let the CRT adapter call `sys_exit(main(argc, argv))`.

Direct `_start(argc, argv)` programs remain supported for low-level probes and
freestanding tests. There is no `envp` argument today; a future runtime can add
`main(argc, argv, envp)` above the same kernel entry ABI when the environment
model exists.

Programs are linked at fixed virtual address `0x400000`, loaded into private user mappings, and entered through `iret` into CPL=3. They use the `int 0x80` syscall ABI for kernel services.

---

# Physical Memory Layout

```text
0x00007C00   bootloader stage 1
0x00040000   loader2 stage 2 (done after protected-mode jump)
0x00001000   kernel image
0x00090000   loader-written boot info
0x00091000   copied BIOS font
0x00100000   kernel .bss start (NOLOAD; page tables, PMM bitmap, static buffers)
~0x00190000  kernel .bss end (depends on static buffers)
~0x00190000  bump allocator base — permanent kernel structures
0x001FF000   KERNEL_BOOT_STACK_TOP (defined in `memory.h`) — boot stack top
             (grows downward; fallback ESP0 for kernel tasks such as the shell)
               kmalloc()      — long-lived kernel-owned data only
0x00200000   PMM base — reclaimable frames
               initialized from E820 usable RAM, with SmallOS reservations
               marked used again
               pmm_alloc_frame() — process_t structs, process PDs,
                                   ELF segment frames, user stack frames,
                                   all process-private page tables,
                                   per-process kernel stack frames
0x08000000   PMM ceiling (128 MB cap; default 32 MB guests expose less via E820)
0x00400000   USER_CODE_BASE — user ELF virtual address (per-process mapping)
0xBFFFF000   user stack virtual address (per-process mapping)
```

---

# Virtual Address Layout Per Process

```text
0x00000000 – 0x003FFFFF   shared supervisor-only mappings (inaccessible from ring 3)
0x00400000 – 0x007FFFFF   user ELF segments (private, PAGE_USER | PAGE_WRITE)
0xBFFFF000                user stack page (private, PAGE_USER | PAGE_WRITE)
0xC0000000 – 0xFFFFFFFF   shared kernel mappings (supervisor-only)
0xE0000000 – 0xE7DFFFFF   shared kernel PMM alias, mapping physical
                          0x00200000 – 0x07FFFFFF
```

## Process Paging Ownership

User processes own their paging structures:

* Page directory — PMM-allocated, freed on process exit
* User-space page tables — PMM-allocated, freed on process exit
* User-space frames (ELF, stack) — PMM-allocated, freed on process exit

Kernel mappings remain shared from `kernel_page_directory` and are never reclaimed by per-process teardown.

`process_pd_destroy()` walks the user PDE range, frees all mapped user frames, frees each private user page table, then frees the page directory frame itself.

PMM-owned memory is stored and passed around as physical frame addresses.
Kernel code dereferences that memory through `paging_phys_to_kernel_virt()`;
page tables and CR3 continue to contain physical addresses. The low identity
map remains for boot compatibility and early kernel-owned memory, not as the
PMM-frame access API.

---

# ext2 Partition

A 16 MB ext2 volume is appended to the disk image directly after the kernel.
The seeded tree uses normal ext2 native directory entries under `/bin`,
`/usr/bin`, `/usr/libexec/tests`, `/usr/sbin`, `/usr/share/examples/tinycc`,
`/etc`, `/var`, and `/tmp`.

Built by `tools/mkext2.c` — a host C tool with no external filesystem tooling
required. The tool writes the ext2 superblock, group descriptor, block and
inode bitmaps, inode table, directory entries, and file data directly.

Volume layout (within the partition image):

```text
byte offset 1024    ext2 superblock
block 1             block group descriptor table
block 2             block bitmap
block 3             inode bitmap
blocks 4-11         inode table
block 12+           file and directory data blocks
```

The ext2 start LBA is computed during final image assembly by `mkimage` as `kernel_lba + kernel_sectors` and written into partition entry 1 of the MBR partition table. `loader2.asm` reads partition entry 0 to load the kernel. At runtime, `ext2_init()` reads ATA sector 0, extracts the ext2 partition metadata, and uses it to locate the live ext2 volume.

Verified by `make image-layout-check`: partition entry 1 has type `0x83` and
points at the appended ext2 image; the runtime then validates magic `0xEF53` in
the ext2 superblock.

---

# ATA Disk Driver

`src/drivers/ata.c` provides 28-bit LBA sector reads and writes from the primary
ATA channel. It discovers PCI IDE bus-master registers and uses bounce-buffered
DMA when available, then falls back to the original port-I/O polling path if DMA
is unavailable or fails. QEMU emulates the primary channel at `0x1F0`.

```text
ata_init()
  outb(0x3F6, 0x04)    SRST = 1 (software reset)
  400ns delay
  outb(0x3F6, 0x00)    SRST = 0
  poll BSY until clear

ata_read_sectors(lba, count, buf)
  poll BSY
  write drive register: 0xE0 | (lba >> 24) & 0xF   (master, LBA mode)
  write sector count, LBA0/1/2
  outb(command, 0x20)   READ SECTORS
  for each sector:
    400ns delay
    poll DRQ (abort on ERR/DF)
    inw × 256           read 512 bytes via 16-bit data register
```

---

# ELF Execution Model

```text
runelf usr/bin/hello arg1
  ↓
vfs_load_file("hello", &size)
  → backend lookup, follow block chain, load into s_load_buf
  ↓
elf_run_image(data, argc, argv)
  ↓
validate ELF magic
process_create("elf")           → allocate process_t from PMM
process_pd_create()             → fresh physical PD frame (pmm_alloc_frame), kernel entries shared
map ELF segments                → pmm_alloc_frame() per page, PAGE_USER at 0x400000
map user stack                  → pmm_alloc_frame(), PAGE_USER at 0xBFFFF000
alloc kernel stack              → pmm_alloc_frame(), per-process ring-0 stack for syscalls/interrupts
elf_seed_sched_context()        → copy argv into process_t.user_arg_data, set proc->user_entry, build proc->sched_esp
                                   seeded to re-enter via elf_user_task_bootstrap()
proc->state = RUNNING
sched_enqueue(proc)             → child becomes a runnable task
process_set_foreground(proc)    → foreground reader/group for interactive waits

[runelf waits with process_wait(proc); runelf_nowait returns immediately]

scheduler enters child via elf_user_task_bootstrap()
  tss_set_kernel_stack(proc->kernel_stack_frame + PAGE_SIZE)
  paging_switch(proc->pd)
  iret → ring 3 with IF set
  ↓
[program runs at CPL=3; timer schedules it normally]
  ↓
sys_exit() → int 0x80 → sched_exit_current((unsigned int)regs)
  paging_switch(kernel_pd)
  proc->state = ZOMBIE
  sched_dequeue(cur)
  switch to next runnable task
  process_destroy(proc) later   → waiter reaps from a safe stack
```

---

# SYS_EXEC

A running user process can invoke a named child through the same ELF creation path:

```text
sys_exec("hello", argc, argv)   [ring-3 call via int 0x80]
  ↓
sys_exec_impl()
  copy name from user address space to kernel stack buffer
  ↓
elf_run_named(kname, argc, argv)
  ↓
elf_run_image()                 [create process, allocate kernel stack, seed bootstrap, enqueue]
  process_claim_for_wait(child)
  ↓
sys_exec_impl returns child pid
  ↓
isr128_stub iretd → parent resumes in ring 3 and may waitpid(pid)
```

This is async spawn, not blocking foreground execution.

---

# Build System

## Disk image layout

```text
LBA 0                     boot.bin
LBA 1 ... loader2_sectors loader2.bin
LBA kernel_lba ... N      padded kernel region
LBA N+1 ...               ext2.img
```

`kernel.bin` is padded to a sector boundary during final image assembly by `mkimage`. `kernel_lba` is derived from the actual loader2 size, and `ext2_LBA = kernel_lba + kernel_sectors` is written into partition entry 1 of the MBR partition table.

## Key generated artifacts

```text
build/gen/<backend>/loader2.gen.asm
                                stack-top values injected
build/bin/<backend>/ext2.seed.img
                                16 MB seeded ext2 image built by build/tools/mkext2
.state/ext2.img             mutable ext2 working copy used by normal runs
build/tools/mkext2          host tool for ext2 volume construction
build/tools/mkimage          host tool for final disk image assembly
build/obj/<backend>/kernel/setjmp.o
                                assembled from src/kernel/setjmp.asm
build/obj/<backend>/kernel/sched_switch.o
                                assembled from src/kernel/sched_switch.asm
```

---

# Known Limitations

* ELF link address fixed at 0x400000 — no PIE/relocation support
* `SYS_EXEC` is async spawn; userland receives a pid and can collect it with `waitpid()`

---

# Debugging Map

| Stage              | Tool                                                    |
| ------------------ | ------------------------------------------------------- |
| boot (real mode)   | BIOS print (int 0x10)                                   |
| early kernel       | VGA direct (0xB8000)                                    |
| kernel             | terminal_puts (serial plus active backend)              |
| user process       | sys_write / sys_putc / sys_read                         |
| memory accounting  | meminfo command                                         |
| BIOS memory map    | memmap command                                          |
| disk reads         | ataread <lba> command                                   |
| crash analysis     | QEMU -d int,cpu_reset,guest_errors -D qemu.log          |

---

# Summary

SmallOS is currently:

```text
two-stage bootloader (CHS + LBA)
paging enabled (identity-mapped 8 MB)
GDT with ring-3 user segments and TSS
process_t abstraction — per-process struct (PD, kernel stack, exit ctx, sched_esp, argv storage, name)
per-process page directories — address space isolation, fully reclaimed on exit
ring-3 ELF execution (hardware privilege enforcement)
int 0x80 syscall interface (DPL=3 gate, TSS stack switch)
negative errno syscall convention — kernel returns raw `-errno`; POSIX user wrappers translate to `-1` plus `errno`
argv copied into process_t kernel storage before CR3 switches
clean process exit via `PROCESS_STATE_ZOMBIE` transition and later reap from a safe stack
physical memory manager (bitmap, all frames reclaimed on exit — no leak)
per-process kernel stacks (PMM frame per process, freed on exit)
SYS_READ / fd 0 — true blocking keyboard input through the console handle: parks process in PROCESS_STATE_WAITING, woken by keyboard IRQ via process_key_consumer()
SYS_MOUSE_READ — polling PS/2 mouse state for graphics demos: returns accumulated relative deltas/buttons and clears the movement counters
SYS_YIELD — voluntary preemption via sched_yield_now()
SYS_SLEEP — timed sleep: parks process in PROCESS_STATE_SLEEPING and wakes via the timer IRQ once the deadline is reached
SYS_EXEC / SYS_WAITPID / SYS_KILL — async ELF spawn returns a child pid; userland can collect exit/signal status or terminate a child by pid
SYS_GETCWD / SYS_CHDIR — per-process cwd state; relative user paths are normalized before VFS or ELF loading
SYS_OPEN / SYS_OPEN_MODE / SYS_CLOSE / SYS_FREAD — dynamic PMM-backed per-process handle table backed by readable/writable handle ops; fd 0/1/2 are console handles, user-opened files start at fd 3+, and VFS-backed file reads cache ext2 data in PMM-backed pages until close
SYS_BRK / user heap — per-process heap break managed in user space through `SYS_BRK` and a shared user allocator
SYS_OPEN_WRITE / SYS_WRITEFD / SYS_LSEEK / SYS_FSYNC / SYS_UNLINK / SYS_RENAME / SYS_STAT — VFS-backed writable file handles plus path metadata and file management for compiler-style tools; dirty writable handles flush on close, append/read-write modes preserve existing bytes, and stdout/stderr writes also use fd-backed console handles
SYS_SOCKET / SYS_BIND / SYS_LISTEN / SYS_ACCEPT / SYS_ACCEPT4 / SYS_CONNECT / SYS_SEND / SYS_RECV / SYS_SHUTDOWN / SYS_GETSOCKNAME / SYS_GETPEERNAME — socket ABI for passive TCP servers, FTP userland, and client-style active opens; fd handles point at kernel socket objects, TCP streams are backed by a global 4-tuple TCP table plus lazy 4 KiB RX rings and 16 KiB TX rings, basic `shutdown()` half-close state is implemented, and socket readiness plugs into the same handle poll path
SYS_FCNTL / SYS_POLL / SYS_EPOLL_* / SYS_TIMERFD_* / SYS_SIGNALFD — descriptor flags and event-loop shims for cserve-style guest services; socket waits register on socket wait queues, timerfd handles register read waiters that timer IRQs can wake when timerfds expire, and signalfd handles can be woken by kernel SIGINT/SIGTERM delivery
TCP service task — drains NIC RX, dispatches ARP/IPv4/TCP frames, advertises receive windows from per-connection RX rings, services retransmit/idle timers for control handshakes, buffered TX payloads, active-open SYNs, and FIN paths, handles duplicate peer-FIN ACKs and close-driven final writes, and wakes socket wait queues
page-aware copy-from-user validation — syscall pointer arguments are checked against user address space [USER_CODE_BASE, USER_STACK_TOP), mapped user pages, page-crossing buffers/structs, and wrapped variable-length byte counts before dereference
preemptive round-robin scheduler — timer IRQ context switch, `SCHED_QUANTUM_MS` quantum
ATA disk driver — 28-bit LBA reads/writes from primary IDE channel (0x1F0), with bus-master DMA and PIO fallback
ext2 filesystem — ELF programs loaded from 16 MB ext2 partition on disk
run/runimg infrastructure removed — `runelf` is the primary external program path, and `SYS_EXEC` reuses that same foreground ELF execution machinery
interactive shell with meminfo / memmap / ataread / fsls / tree / fsread / mkdir / rmdir / runelf commands
guest TinyCC compiler path — `usr/bin/tcc.elf` runs inside SmallOS through `user_crt0` and TinyCC's normal `main`, then compiles guest C samples during `make test`
```

Foundation for:

* per-element `argv[]` validation in `SYS_EXEC`
