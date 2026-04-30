#include "idt.h"
#include "ports.h"
#include "terminal.h"
#include "keyboard.h"
#include "timer.h"
#include "paging.h"
#include "process.h"
#include "scheduler.h"

extern void idt_flush(unsigned int);
extern void irq0_stub(void);
extern void irq1_stub(void);
extern void isr0_stub(void);
extern void isr5_stub(void);
extern void isr6_stub(void);
extern void isr13_stub(void);
extern void isr14_stub(void);
extern void isr8_stub(void);
extern void isr128_stub(void);

static struct idt_entry idt[256];
static struct idt_ptr idtp;

void idt_set_gate(unsigned char num, unsigned int base, unsigned short sel, unsigned char flags) {
    idt[num].base_low = base & 0xFFFF;
    idt[num].sel = sel;
    idt[num].always0 = 0;
    idt[num].flags = flags;
    idt[num].base_high = (base >> 16) & 0xFFFF;
}

static void pic_remap(void) {
    unsigned char a1 = inb(0x21);
    unsigned char a2 = inb(0xA1);

    outb(0x20, 0x11);
    io_wait();
    outb(0xA0, 0x11);
    io_wait();

    outb(0x21, 0x20);
    io_wait();
    outb(0xA1, 0x28);
    io_wait();

    outb(0x21, 0x04);
    io_wait();
    outb(0xA1, 0x02);
    io_wait();

    outb(0x21, 0x01);
    io_wait();
    outb(0xA1, 0x01);
    io_wait();

    outb(0x21, a1);
    outb(0xA1, a2);
}

void idt_init(void) {
    for (int i = 0; i < 256; i++) {
        idt_set_gate(i, 0, 0, 0);
    }

    pic_remap();

    idt_set_gate(0,   (unsigned int)isr0_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(5,   (unsigned int)isr5_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(6,   (unsigned int)isr6_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(13,  (unsigned int)isr13_stub,  KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(14,  (unsigned int)isr14_stub,  KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(8,   (unsigned int)isr8_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(32,  (unsigned int)irq0_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);
    idt_set_gate(33,  (unsigned int)irq1_stub,   KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_KERNEL);

    /*
     * int 0x80 syscall gate — DPL=3 so ring-3 code can invoke it.
     *
     * IDT_FLAG_INT_GATE_KERNEL (0x8E) has DPL=0: ring-3 hitting it
     * causes a #GP fault before the handler even runs.
     * IDT_FLAG_INT_GATE_USER  (0xEE) has DPL=3: the CPU allows the
     * software interrupt from any privilege level.
     *
     * The handler itself (isr128_stub → syscall_handler_main) always
     * runs in ring 0 because it is reached via an interrupt gate, which
     * clears IF and switches to the kernel code selector.
     */
    idt_set_gate(128, (unsigned int)isr128_stub, KERNEL_CS_SELECTOR, IDT_FLAG_INT_GATE_USER);

    idtp.limit = sizeof(struct idt_entry) * 256 - 1;
    idtp.base = (unsigned int)&idt;

    idt_flush((unsigned int)&idtp);

    outb(0x21, 0xFC);
    outb(0xA1, 0xFF);
}

static unsigned int fault_frame_word(unsigned int esp, unsigned int index) {
    return ((unsigned int*)esp)[index];
}

static unsigned int pf_get_cr2(void) {
    unsigned int cr2;
    __asm__ __volatile__("mov %%cr2, %0" : "=r"(cr2));
    return cr2;
}

static void fault_handler_common(const char* tag, unsigned int esp, unsigned int has_err, unsigned int has_cr2) {
    unsigned int err = has_err ? fault_frame_word(esp, 12) : 0;
    unsigned int eip = fault_frame_word(esp, has_err ? 13 : 12);
    unsigned int cs = fault_frame_word(esp, has_err ? 14 : 13);
    unsigned int cr2 = has_cr2 ? pf_get_cr2() : 0;
    process_t* proc = sched_current();

    terminal_puts(tag);
    terminal_puts(" eip=");
    terminal_put_hex(eip);
    terminal_puts(" cs=");
    terminal_put_hex(cs);
    terminal_puts(((cs & 3u) == 3u) ? " user" : " kernel");
    if (has_err) {
        terminal_puts(" err=");
        terminal_put_hex(err);
    }
    if (has_cr2) {
        terminal_puts(" cr2=");
        terminal_put_hex(cr2);
    }
    terminal_putc('\n');

    /*
     * User faults terminate just the current process so the shell and
     * the rest of the VM stay alive.  Kernel faults still halt hard so
     * we preserve the last error context instead of trying to recover
     * from a potentially corrupted kernel stack.
     */
    if (proc && proc->pd != 0 && (cs & 3u) == 3u) {
        terminal_puts(tag);
        terminal_puts(" term ");
        terminal_puts(proc->name);
        terminal_putc('\n');

        paging_switch(paging_get_kernel_pd());
        sched_exit_current(esp);
    }

    terminal_puts(tag);
    terminal_puts(" kernel panic\n");
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

void invalid_opcode_handler_main(unsigned int esp) {
    fault_handler_common("ud", esp, 0, 0);
}

void divide_error_handler_main(unsigned int esp) {
    fault_handler_common("de", esp, 0, 0);
}

void bound_range_handler_main(unsigned int esp) {
    fault_handler_common("br", esp, 0, 0);
}

void general_protection_handler_main(unsigned int esp) {
    fault_handler_common("gp", esp, 1, 0);
}

void page_fault_handler_main(unsigned int esp) {
    fault_handler_common("pf", esp, 1, 1);
}

/*
 * irq0_handler_main(esp)
 *
 * Timer IRQ handler.  esp is the kernel stack pointer at the point
 * irq0_stub called us — it points at the saved register frame.
 *
 * We must send EOI before calling sched_tick, because sched_switch may
 * resume a different context that never returns through this function.
 * If EOI were sent after sched_tick, the outgoing context would have
 * its EOI sent when it is eventually rescheduled — but the incoming
 * context would run with IRQ0 still masked in the PIC, meaning no
 * further timer ticks until the original context runs again.
 */
#define SCHED_RESUME_RETADDR_OFFSET 8u

void irq0_handler_main(unsigned int esp) {
    timer_handle_irq();
    outb(0x20, 0x20);   /* EOI before sched_tick — see above */
    sched_tick(esp - SCHED_RESUME_RETADDR_OFFSET);
}

void irq1_handler_main(void) {
    outb(0x20, 0x20);   /* EOI first — keep PIC unmasked before IRQ-side work */
    keyboard_handle_irq();
}
