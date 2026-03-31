# Interrupt Architecture

This document defines how interrupts are delivered, handled, and dispatched in the system.

---

# Overview

```text
hardware interrupt / software interrupt
  ↓
IDT entry
  ↓
assembly stub (interrupts.asm)
  ↓
C handler
  ↓
subsystem (timer / keyboard / syscall)
```

---

# IDT (Interrupt Descriptor Table)

Defined in `src/kernel/idt.c`. Each entry contains a handler address, code segment selector, and flags.

```c
idt_set_gate(vector, handler, 0x08, flags);
```

## Active Entries

```text
8    → ISR8 (double fault)
32   → IRQ0 (timer)
33   → IRQ1 (keyboard)
128  → syscall (int 0x80)
```

---

# PIC Remapping

IRQs 0–15 are remapped away from CPU exception vectors 0–15.

```text
IRQ0 → vector 32
IRQ1 → vector 33
```

---

# Interrupt Stubs (interrupts.asm)

## IRQ1 stub (keyboard) — standard pattern

```asm
pusha
push ds / es / fs / gs
mov ax, 0x10          ; load kernel data segment
mov ds/es/fs/gs, ax
call irq1_handler_main
pop gs / fs / es / ds
popa
iretd
```

## IRQ0 stub (timer) — passes ESP to C

`irq0_stub` uses the same frame layout but pushes ESP and passes it to C, mirroring `isr128_stub`. This gives the scheduler the kernel stack pointer it needs to save and restore contexts.

```asm
pusha
push ds / es / fs / gs
mov ax, 0x10
mov ds/es/fs/gs, ax
push esp                  ; pass register-frame pointer to C
call irq0_handler_main    ; void irq0_handler_main(unsigned int esp)
add esp, 4
pop gs / fs / es / ds
popa
iretd
```

Stack layout after push (used by the scheduler as the resume point):

```text
[esp]    gs   (lowest address — last pushed)
[esp+4]  fs
[esp+8]  es
[esp+12] ds
[esp+16] edi  \
[esp+20] esi   |
[esp+24] ebp   | pusha frame
[esp+28] esp0  |  (original ESP before pusha)
[esp+32] ebx   |
[esp+36] edx   |
[esp+40] ecx   |
[esp+44] eax  /
[esp+48] eip  \  pushed by CPU on interrupt
[esp+52] cs    |
[esp+56] eflags/
(+ esp/ss if ring-3 → ring-0 transition)
```

## ISR128 stub (syscall) — same ESP-passing pattern

```asm
pusha
push ds / es / fs / gs
mov ax, 0x10
mov ds/es/fs/gs, ax
push esp
call syscall_handler_main
add esp, 4
pop gs / fs / es / ds
popa
iretd
```

---

# IRQ Handling

## Timer (IRQ0)

```text
vector 32 → irq0_stub → irq0_handler_main(esp)
```

```c
void irq0_handler_main(unsigned int esp) {
    timer_handle_irq();
    outb(0x20, 0x20);   /* EOI before sched_tick */
    sched_tick(esp);
}
```

**EOI ordering**: EOI is sent *before* `sched_tick`. If `sched_tick` calls `sched_switch` and the current invocation of `irq0_handler_main` never returns through the normal path, the PIC is already unmasked. Sending EOI after `sched_tick` would permanently mask IRQ0 for the incoming context until the outgoing one was next scheduled.

## Keyboard (IRQ1)

```text
vector 33 → irq1_stub → irq1_handler_main
```

```c
void irq1_handler_main(void) {
    outb(0x20, 0x20);       /* EOI first — handler may never return */
    keyboard_handle_irq();
}
```

EOI is sent first for the same reason: when the Enter keypress launches a process, `keyboard_handle_irq` → `shell_execute` → `elf_run_image` → `iret` never returns, so EOI must already be sent.

---

# End of Interrupt (EOI)

```c
outb(0x20, 0x20);
```

Required for every IRQ. Without it, the PIC keeps the line in-service and no further interrupts fire.

**Rule**: send EOI at the top of any handler that might not return.

---

# System Call Path (int 0x80)

```text
int 0x80 (CPL=3)
  ↓
CPU checks IDT[128].DPL=3 — allowed from ring 3
CPU loads SS0/ESP0 from TSS — switches to per-process kernel stack
CPU pushes SS, ESP, EFLAGS, CS, EIP
  ↓
isr128_stub: saves full register frame, loads kernel DS=0x10
  push esp → call syscall_handler_main(regs)
  ↓
handler sets regs->eax = return value
  ↓
pop frame, iretd → ring 3 resumes
```

---

# Register Frame Contract (syscall)

The `isr128_stub` push order determines the `syscall_regs_t` field layout. Last pushed = lowest address = first struct field.

```c
typedef struct syscall_regs {
    unsigned int gs;   /* pushed last  → lowest address → first field */
    unsigned int fs;
    unsigned int es;
    unsigned int ds;
    unsigned int edi;
    unsigned int esi;
    unsigned int ebp;
    unsigned int esp;
    unsigned int ebx;
    unsigned int edx;
    unsigned int ecx;
    unsigned int eax;  /* pushed first → highest address → last field */
} syscall_regs_t;
```

**If you modify `isr128_stub`, you must update `syscall_regs_t` to match.**

---

# Scheduler Integration

`irq0_stub` was extended to pass ESP to C specifically to support the scheduler. The kernel ESP at that point is the address of the register frame sitting on the current context's kernel stack. The scheduler records this as `sched_esp` and passes it to `sched_switch` as `next_esp` when resuming the context.

See `scheduler.h` / `sched_switch.asm` for the full context-switch mechanism.

---

# Double Fault Handler (ISR8)

```asm
isr8_stub:
    write '8' to VGA (red)
    cli / hlt loop
```

Halts the system visibly rather than silently rebooting.

---

# Enabling Interrupts

`sti` is called in `kernel_main()` after all of the following are valid:

* GDT loaded and flushed
* IDT loaded
* PIC remapped
* All IRQ handlers installed
* TSS loaded (`ltr 0x28`)
* Scheduler initialised (`sched_init()`)

Any gap in this list causes `#GP → #DF → triple fault → reboot`.

---

# Failure Modes

| Symptom | Likely cause |
|---|---|
| Reboot immediately after `sti` | Bad GDT, bad IDT, or missing segment setup |
| Red '8' on screen | Double fault — corrupt kernel stack or bad IDT entry |
| Timer not advancing | IRQ0 not firing; missing EOI |
| No keyboard input | IRQ1 masked or missing EOI |
| Syscalls silently broken | `syscall_regs_t` mismatch with `isr128_stub` push order |
| Timer fires but process never preempted | `sched_tick` not called; EOI sent after `sched_switch` |
| Crash on first context switch | `sched_esp == 0` guard missing; switching to process with no saved stack |

---

# Summary

```text
interrupt
  → IDT lookup
  → assembly stub (saves frame, loads kernel DS)
    IRQ0: also pushes ESP → passes to C for scheduler
  → C handler
    IRQ0: timer_handle_irq(), EOI, sched_tick(esp)
    IRQ1: EOI, keyboard_handle_irq()
    int 0x80: syscall_handler_main(regs)
  → return (iretd) or context switch via sched_switch
```