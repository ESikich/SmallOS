#ifndef IDT_H
#define IDT_H

#include <stddef.h>

#define IDT_FLAG_INT_GATE_KERNEL 0x8E
#define IDT_FLAG_INT_GATE_USER   0xEE
#define KERNEL_CS_SELECTOR       0x08

struct idt_entry {
    unsigned short base_low;
    unsigned short sel;
    unsigned char  always0;
    unsigned char  flags;
    unsigned short base_high;
} __attribute__((packed));

struct idt_ptr {
    unsigned short limit;
    unsigned int   base;
} __attribute__((packed));

void idt_init(void);
void idt_set_gate(unsigned char num, unsigned int base, unsigned short sel, unsigned char flags);
void irq0_handler_main(unsigned int esp);
void irq1_handler_main(void);

#endif /* IDT_H */