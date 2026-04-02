# Architecture Overview

This document describes how SimpleOS boots, initializes, and executes programs.

---

# Boot Flow

```text
BIOS
  ↓
boot.asm (stage 1, 16-bit)
  ↓
loader2.asm (stage 2, 16-bit → 32-bit)
  loads kernel  → 0x1000
  ↓
kernel_entry.asm (32-bit)
  zeros BSS
  ↓
kernel_main()
  gdt_init()        ← null, k-code, k-data, u-code, u-data, TSS + ltr
  paging_init()     ← identity-maps first 8 MB, enables CR0.PG
  memory_init()     ← bump allocator base at 0x100000
  pmm_init()        ← bitmap allocator at 0x200000–0x7FFFFF
  keyboard/timer/idt
  sched_init()      ← initialise runnable task table
  ata_init()        ← software reset ATA primary channel, verify ready
  fat16_init()      ← read BPB, validate FAT16 volume geometry
  create shell task ← explicit kernel task with dedicated stack
  sti
  sched_start()     ← switch from boot stack into shell task
```

---

## Stage 1 – boot.asm

* Loaded by BIOS at `0x7C00`
* Loads stage 2 via CHS `INT 0x13 AH=0x02` (4 sectors, fits within one track)
* Must be exactly **512 bytes**, ending with `dw 0xAA55`

---

## Stage 2 – loader2.asm

Runs in **real mode**, then switches to **protected mode**.

* Checks `INT 0x13 AH=0x41` for LBA extension support — halts if not available
* Loads kernel (LBA 5) to `0x1000` via `INT 0x13 AH=0x42`
* Sets up temporary GDT, enables protected mode, jumps to `0x1000`

One value injected by Makefile at build time: `KERNEL_SECTORS`.

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

BSS zeroing is mandatory. The three paging structures and the PMM bitmap live in `.bss` (roughly in the low kernel image area after load, not at a hard-coded address guaranteed by the linker script). Without zeroing, `paging_init()` can triple-fault and the PMM bitmap would falsely show all frames as used.

---

# Kernel Initialization

Inside `kernel_main()`:

1. `terminal_init()` — VGA text mode, cursor control
2. `gdt_init()` — install GDT with ring-3 segments and TSS; load task register with `ltr`
3. `paging_init()` — enable paging, identity-map 8 MB
4. `memory_init(0x100000)` — bump allocator starts at 1 MB
5. `pmm_init()` — bitmap allocator covers 0x200000–0x7FFFFF; all frames start free
6. `keyboard_init()`, `timer_init(100)`, `idt_init()` — drivers and interrupt table
7. `sched_init()` — initialise the scheduler data structures
8. `ata_init()` — software reset ATA primary channel (`0x1F0`), poll until ready
9. `fat16_init()` — read FAT16 BPB at `FAT16_LBA` (from sector 0 offset 504), validate volume
10. `process_create_kernel_task("shell", shell_task_main)` — create the shell as an explicit kernel task
11. `sched_enqueue(shell_proc)` — make the shell runnable
12. `sti` — enable interrupts
13. `sched_start(shell_proc)` — switch from the boot stack into the shell task

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
    jmp_buf         exit_ctx;               /* setjmp context for sys_exit return  */
    unsigned int    sched_esp;              /* kernel ESP saved on preemption      */
    process_state_t state;                  /* UNUSED / RUNNING / EXITED          */
    void          (*kernel_entry)(void);    /* kernel task entry point             */
    unsigned int    user_entry;             /* first ring-3 EIP for ELF tasks      */
    int             user_argc;              /* saved argc for bootstrap            */
    char*           user_argv[16];          /* saved argv pointers                 */
    char            user_arg_data[256];     /* argv string storage (kernel-side)   */
    char            name[32];               /* null-terminated process name        */
} process_t;
```

`sched_esp` is 0 until the process has been preempted at least once. The scheduler skips any slot where `state != RUNNING` or `sched_esp == 0`.

`user_arg_data` / `user_argv` hold copies of the argv strings inside the `process_t` PMM frame — independent of the shell input buffer and valid after CR3 switches.

---

# Scheduler

A round-robin preemptive scheduler runs on every timer tick (100 Hz, quantum = 10 ticks = 100 ms).

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
runelf:
  process_create()
  [NOT enqueued in scheduler — runs via foreground path]
  save s_parent_proc / s_parent_esp0
  setjmp / iret into ring 3

sys_exit:
  paging_switch(s_parent_proc->pd or kernel_pd)
  tss_set_kernel_stack(s_parent_esp0)
  process_destroy(proc)
  process_set_current(s_parent_proc)
  longjmp → parent (shell or user process)
```

---

# Interrupt Architecture

## IDT entries

```text
8    → ISR8 (double fault — VGA marker '8' white-on-red at row 1 col 12 + halt)
32   → IRQ0 (timer, DPL=0)
33   → IRQ1 (keyboard, DPL=0)
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
irq1_stub: pusha, push segments
  ↓
irq1_handler_main:
  EOI (outb 0x20, 0x20)        ← sent before handler (consistent with IRQ0 pattern)
  keyboard_handle_irq()
  ↓
irq1_stub: pop segments, popa, iretd
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

---

# Terminal + Shell

## Terminal

VGA text mode (`0xB8000`). Provides `terminal_putc`, `terminal_puts`, `terminal_put_uint`, `terminal_put_hex`, cursor control, scrolling.

## Shell

```text
keyboard IRQ → keyboard_handle_irq()
  ↓
  if kb_process_mode: push to ring buffer (for SYS_READ)
  else: queue shell event
    ↓
    shell_task_main() drains queue via shell_poll()
    ↓
    line_editor (insert, delete, cursor, history)
    ↓
    [Enter] → parse_command() → commands_execute()
    ↓
    runelf / fsls / fsread / ataread / meminfo / ... dispatch
```

---

# User Programs

Entry point convention:

```c
void _start(int argc, char** argv)
```

Programs are linked at fixed virtual address `0x400000`, loaded into private user mappings, and entered through `iret` into CPL=3. They use the `int 0x80` syscall ABI for kernel services.

---

# Physical Memory Layout

```text
0x00007C00   bootloader stage 1
0x0000A000   loader2 stage 2 (done after protected-mode jump)
0x00001000   kernel image
0x00006000   kernel .bss start (page tables + PMM bitmap)
~0x0000A008  kernel .bss end
0x00090000   kernel stack top (grows downward; shell context)
0x00100000   bump allocator base — permanent kernel structures
               kmalloc()      — long-lived kernel-owned data only
0x00200000   PMM base — reclaimable frames
               pmm_alloc_frame() — process_t structs, process PDs,
                                   ELF segment frames, user stack frames,
                                   all process-private page tables,
                                   per-process kernel stack frames
0x00800000   PMM ceiling (= identity-map limit)
0x00400000   USER_CODE_BASE — user ELF virtual address (per-process mapping)
0xBFFFF000   user stack virtual address (per-process mapping)
```

---

# Virtual Address Layout Per Process

```text
0x000000 – 0x3FFFFF   shared supervisor-only mappings (inaccessible from ring 3)
0x400000 – 0x7FFFFF   user ELF segments (private, PAGE_USER | PAGE_WRITE)
0xBFFFF000            user stack page (private, PAGE_USER | PAGE_WRITE)
PD 2–1023 range       kernel heap etc. (shared, supervisor-only)
```

## Process Paging Ownership

User processes own their paging structures:

* Page directory — PMM-allocated, freed on process exit
* User-space page tables — PMM-allocated, freed on process exit
* User-space frames (ELF, stack) — PMM-allocated, freed on process exit

Kernel mappings remain shared from `kernel_page_directory` and are never reclaimed by per-process teardown.

`process_pd_destroy()` walks the user PDE range, frees all mapped user frames, frees each private user page table, then frees the page directory frame itself.

---

# FAT16 Partition

A 16 MB FAT16 volume is appended to the disk image directly after the kernel. It contains all user ELFs in the root directory as 8.3-format filenames.

Built by `tools/mkfat16.c` — a host C tool with no external dependencies (`mkfs.vfat` and `mtools` are not required). The tool writes the BPB, both FAT copies, the root directory, and all file data directly.

Volume layout (within the partition image):

```text
Sector   0        Boot sector (BPB) — OEM "SIMPLEOS", FAT16 signature 0x55 0xAA
Sectors  1–3      Reserved (4 reserved sectors total)
Sectors  4–35     FAT 1  (32 sectors, 8192 FAT16 entries)
Sectors 36–67     FAT 2  (mirror)
Sectors 68–99     Root directory (512 entries × 32 bytes = 32 sectors)
Sectors 100+      Data region  (cluster 2 = sectors 100–103, etc.)
```

`FAT16_LBA` is computed at build time as `5 + kernel_sectors` and patched as a little-endian u32 into byte offset 504 of the boot sector. `fat16_init()` reads ATA sector 0 and extracts it.

Verified at runtime: `ataread <FAT16_LBA>` shows `EB 58 90 SIMPLEOS` and `0x55 0xAA`; `ataread <FAT16_LBA + 100>` shows `7F 45 4C 46` (ELF magic at cluster 2).

---

# ATA PIO Driver

`src/drivers/ata.c` provides 28-bit LBA sector reads from the primary ATA channel using port I/O polling. No DMA, no IRQ required. QEMU emulates the primary channel at `0x1F0`.

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
runelf hello arg1
  ↓
fat16_load("hello", &size)
  → search FAT16 root directory, follow cluster chain, load into s_load_buf
  ↓
elf_run_image(data, argc, argv)
  ↓
validate ELF magic
process_create("elf")           → allocate process_t from PMM
process_pd_create()             → fresh PD (pmm_alloc_frame), kernel entries shared
map ELF segments                → pmm_alloc_frame() per page, PAGE_USER at 0x400000
map user stack                  → pmm_alloc_frame(), PAGE_USER at 0xBFFFF000
alloc kernel stack              → pmm_alloc_frame(), tss_set_kernel_stack(frame + PAGE_SIZE)
elf_seed_sched_context()        → copy argv into process_t.user_arg_data
save parent context             → s_parent_proc = process_get_current()
                                   s_parent_esp0 = parent ? parent->ksf+PAGE_SIZE : 0x90000
process_set_current(proc)
proc->state = RUNNING

[NOT enqueued in scheduler — runs via foreground path]

setjmp(proc->exit_ctx)          → save kernel context for sys_exit return
keyboard_set_process_mode(1)
paging_switch(proc->pd)
iret → ring 3
  ↓
[program runs at CPL=3; timer still ticks but does not schedule this process]
  ↓
sys_exit() → int 0x80 → elf_process_exit()
  proc->state = EXITED
  keyboard_set_process_mode(0)
  paging_switch(s_parent_proc->pd or kernel_pd)   ← parent's PD if user process
  tss_set_kernel_stack(s_parent_esp0)
  process_destroy(proc)
  process_set_current(s_parent_proc)
  sti
  longjmp(exit_ctx, 1)          → resume in elf_run_image → return to parent
```

---

# SYS_EXEC

A running user process can spawn a named child:

```text
sys_exec("hello", argc, argv)   [ring-3 call via int 0x80]
  ↓
sys_exec_impl()
  copy name from user address space to kernel stack buffer
  ↓
elf_run_named(kname, argc, argv)
  ↓
elf_run_image()                 [full launch path above]
  ↓
[child runs; parent suspended at setjmp in elf_run_image on parent's kernel stack]
  ↓
child calls sys_exit() → elf_process_exit()
  paging_switch(parent->pd)
  tss_set_kernel_stack(parent->kernel_stack_frame + PAGE_SIZE)
  process_destroy(child)
  process_set_current(parent)
  sti
  longjmp(child->exit_ctx, 1)
  ↓
setjmp returns → elf_run_image returns 1 → sys_exec_impl returns 0
  ↓
isr128_stub iretd → parent resumes in ring 3
```

---

# Build System

## Disk image layout

```text
LBA 0         boot.bin              (512 bytes)
LBA 1–4       loader2.bin           (2048 bytes)
LBA 5+        kernel_padded.bin     (sector-aligned)
LBA 5+ks      fat16.img             (16 MB FAT16 partition)
```

`kernel.bin` is padded to a 512-byte sector boundary before concatenation. `FAT16_LBA = 5 + kernel_sectors` is patched into boot sector offset 504 after image assembly.

## Key generated artifacts

```text
build/gen/loader2.gen.asm    KERNEL_SECTORS injected
build/bin/kernel_padded.bin  kernel.bin padded to sector boundary
build/bin/fat16.img          16 MB FAT16 image built by build/tools/mkfat16
build/tools/mkfat16          host tool (no external FS dependencies)
build/obj/setjmp.o           assembled from src/kernel/setjmp.asm
build/obj/sched_switch.o     assembled from src/kernel/sched_switch.asm
```

---

# Known Limitations

* ELF link address fixed at 0x400000 — no PIE/relocation support
* `SYS_EXEC` is one-deep — `s_parent_proc`/`s_parent_esp0` are single statics
* Kernel trusts user pointers in syscalls (no copy-from-user validation)
* ELF processes are not yet scheduler-owned tasks

---

# Future Architecture Direction

1. Enqueue ELF processes into the scheduler as full tasks

---

# Debugging Map

| Stage              | Tool                                                    |
| ------------------ | ------------------------------------------------------- |
| boot (real mode)   | BIOS print (int 0x10)                                   |
| early kernel       | VGA direct (0xB8000)                                    |
| kernel             | terminal_puts                                           |
| user process       | sys_write / sys_putc / sys_read                         |
| memory accounting  | meminfo command                                         |
| disk reads         | ataread <lba> command                                   |
| crash analysis     | QEMU -d int,cpu_reset,guest_errors -D qemu.log          |

---

# Summary

SimpleOS is currently:

```text
two-stage bootloader (CHS + LBA)
paging enabled (identity-mapped 8 MB)
GDT with ring-3 user segments and TSS
process_t abstraction — per-process struct (PD, kernel stack, exit ctx, sched_esp, argv storage, name)
per-process page directories — address space isolation, fully reclaimed on exit
ring-3 ELF execution (hardware privilege enforcement)
int 0x80 syscall interface (DPL=3 gate, TSS stack switch)
argv copied into process_t kernel storage before CR3 switches
clean process exit via setjmp/longjmp with parent context restore
physical memory manager (bitmap, all frames reclaimed on exit — no leak)
per-process kernel stacks (PMM frame per process, freed on exit)
SYS_READ — blocking keyboard input for user programs
SYS_YIELD — voluntary preemption via sched_yield_now()
SYS_EXEC — nested foreground ELF execution with full parent context save/restore
preemptive round-robin scheduler — timer IRQ context switch, 100 ms quantum
ATA PIO driver — 28-bit LBA polling reads from primary IDE channel (0x1F0)
FAT16 filesystem — ELF programs loaded from 16 MB FAT16 partition on disk
run/runimg/exec infrastructure removed — runelf is the only execution path
interactive shell with meminfo / ataread / fsls / fsread / runelf commands
```

Foundation for:

* ELF processes as scheduler-owned tasks
* richer filesystem-backed program loading
* cleaner separation between kernel tasks and user tasks