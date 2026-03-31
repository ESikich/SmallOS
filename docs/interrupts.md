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

Defined in:

```text
src/kernel/idt.c
```

Each entry contains:

* handler address
* code segment selector
* flags

Initialization:

```c
idt_set_gate(vector, handler, 0x08, 0x8E);
```

---

## Active Entries

```text
8    → ISR8 (double fault)
32   → IRQ0 (timer)
33   → IRQ1 (keyboard)
128  → syscall (int 0x80)
```

---

# PIC Remapping

The PIC is remapped to avoid conflicts with CPU exceptions.

Default IRQ range:

```text
0–15 → conflicts with CPU exceptions
```

Remapped:

```text
IRQ0 → 32
IRQ1 → 33
```

---

## PIC Initialization

Sequence:

```c
outb(0x20, 0x11);
outb(0xA0, 0x11);

outb(0x21, 0x20);
outb(0xA1, 0x28);

outb(0x21, 0x04);
outb(0xA1, 0x02);

outb(0x21, 0x01);
outb(0xA1, 0x01);
```

---

# Interrupt Stubs (Assembly)

Defined in:

```text
src/kernel/interrupts.asm
```

Each stub performs:

1. Save registers
2. Switch to kernel segments
3. Call C handler
4. Restore registers
5. Return via `iretd`

---

## IRQ Stub Pattern

```asm
pusha
push ds
push es
push fs
push gs

mov ax, 0x10
mov ds, ax
mov es, ax
mov fs, ax
mov gs, ax

call handler

pop gs
pop fs
pop es
pop ds
popa
iretd
```

---

# IRQ Handling

## Timer (IRQ0)

```text
vector 32 → irq0_stub → irq0_handler_main
```

Handler:

```c
timer_handle_irq();
outb(0x20, 0x20);
```

Responsibilities:

* increment tick counter
* acknowledge interrupt

---

## Keyboard (IRQ1)

```text
vector 33 → irq1_stub → irq1_handler_main
```

Handler:

```c
keyboard_handle_irq();
outb(0x20, 0x20);
```

Responsibilities:

* read scancode
* update input buffer

---

# End of Interrupt (EOI)

Required for all IRQs.

```c
outb(0x20, 0x20);
```

Without EOI:

* interrupts stop firing
* system appears frozen

---

# System Call Path (int 0x80)

```text
int 0x80
  ↓
isr128_stub
  ↓
syscall_handler_main
```

---

## ISR Stub (syscall)

Key difference from IRQ:

* passes register frame to C

```asm
pusha
push ds
push es
push fs
push gs

push esp
call syscall_handler_main
add esp, 4
```

---

# Register Frame Contract

Stack layout must match:

```c
typedef struct syscall_regs {
    unsigned int gs;   // pushed last → lowest address → first field
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
    unsigned int eax;  // pushed first by pusha → highest address → last field
} syscall_regs_t;
```

Because the stack grows downward, the last item pushed (`gs`) sits at the lowest address. The pointer passed to C points there, so the struct reads fields in reverse push order.

---

## Constraint

The assembly push order defines the struct layout.

Any deviation breaks:

* syscall arguments
* return values

---

# Double Fault Handler (ISR8)

Defined as:

```asm
isr8_stub:
    write '8' to VGA
    halt
```

Purpose:

* detect catastrophic failure
* prevent silent reboot

---

# Enabling Interrupts

Interrupts are enabled with:

```asm
sti
```

---

## Hard Requirement

Before `sti`, the following must be valid:

* GDT
* IDT
* segment registers
* IRQ handlers

Failure results in:

```text
#GP → #DF → triple fault → reset
```

---

# GDT Dependency

Interrupt handling requires a valid GDT.

The kernel installs its own GDT.

The bootloader GDT is not sufficient once interrupts are enabled.

---

# Failure Modes

## Immediate reboot after `sti`

Cause:

* invalid GDT
* invalid IDT
* bad segment selectors

---

## No keyboard input

Cause:

* IRQ1 not enabled
* missing EOI
* handler not installed

---

## Timer not advancing

Cause:

* IRQ0 not firing
* missing EOI

---

## Syscalls fail silently

Cause:

* register frame mismatch
* incorrect stub

---

# Design Notes

* All interrupts run in ring 0
* No interrupt nesting control
* No preemption yet
* Handlers are minimal and synchronous

---

# Future Work

* interrupt masking
* nested interrupt handling
* preemptive scheduling
* user-mode interrupt transitions
* separate syscall gate (ring 3)

---

# Summary

Interrupt flow:

```text
interrupt
 → IDT lookup
 → assembly stub
 → C handler
 → subsystem logic
 → EOI
 → return (iretd)
```

The system depends on:

* correct descriptor tables
* consistent register handling
* strict ABI adherence