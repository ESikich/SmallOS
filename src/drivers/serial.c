#include "serial.h"

#define COM1_BASE   0x3F8
#define COM1_DATA   (COM1_BASE + 0)
#define COM1_IER    (COM1_BASE + 1)
#define COM1_FCR    (COM1_BASE + 2)
#define COM1_LCR    (COM1_BASE + 3)
#define COM1_MCR    (COM1_BASE + 4)
#define COM1_LSR    (COM1_BASE + 5)

#define LSR_TX_EMPTY  0x20

static inline void outb(unsigned short port, unsigned char val) {
    __asm__ __volatile__("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline unsigned char inb(unsigned short port) {
    unsigned char val;
    __asm__ __volatile__("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

void serial_init(void) {
    outb(COM1_IER, 0x00);   /* disable interrupts */
    outb(COM1_LCR, 0x80);   /* enable DLAB to set baud rate */
    outb(COM1_DATA, 0x01);  /* divisor low:  115200 baud */
    outb(COM1_IER,  0x00);  /* divisor high: 0 */
    outb(COM1_LCR, 0x03);   /* 8N1, DLAB off */
    outb(COM1_FCR, 0xC7);   /* enable + clear FIFO, 14-byte threshold */
    outb(COM1_MCR, 0x03);   /* DTR + RTS */
}

void serial_putc(char c) {
    outb(COM1_DATA, (unsigned char)c);
}
