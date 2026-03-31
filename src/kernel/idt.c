#include "idt.h"
#include "ports.h"
#include "keyboard.h"
#include "timer.h"

extern void idt_flush(unsigned int);
extern void irq0_stub(void);
extern void irq1_stub(void);
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

void irq0_handler_main(void) {
    timer_handle_irq();
    outb(0x20, 0x20);
}

void irq1_handler_main(void) {
    outb(0x20, 0x20);   /* EOI first — keyboard_handle_irq may never return */
    keyboard_handle_irq();
}