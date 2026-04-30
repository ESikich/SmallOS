#include "terminal.h"
#include "screen.h"
#include "serial.h"

void terminal_init(void) {
    serial_init();
    terminal_clear();
}

void terminal_clear(void) {
    screen_clear();
}

void terminal_putc(char c) {
    screen_putc(c);
    serial_putc(c);
}

void terminal_puts(const char* s) {
    while (*s)
        terminal_putc(*s++);
}

/*
 * terminal_put_uint(value)
 *
 * Print an unsigned decimal integer.  Uses a small stack buffer, fills it
 * right-to-left with digits, then prints forward — no reversal loop needed.
 */
void terminal_put_uint(unsigned int value) {
    char buf[12];          /* 2^32 = 10 digits max, +1 for null */
    int  pos = 11;
    buf[pos] = '\0';

    if (value == 0) {
        terminal_putc('0');
        return;
    }

    while (value > 0 && pos > 0) {
        buf[--pos] = (char)('0' + value % 10);
        value /= 10;
    }

    terminal_puts(&buf[pos]);
}

/*
 * terminal_put_hex(value)
 *
 * Print an unsigned integer in uppercase hex with "0x" prefix.
 * E.g. 0x100000, 0xBFFFF000.
 */
void terminal_put_hex(unsigned int value) {
    static const char hex[] = "0123456789ABCDEF";
    char buf[11];          /* "0x" + 8 hex digits + null */
    int  pos = 10;
    buf[pos] = '\0';

    if (value == 0) {
        terminal_puts("0x0");
        return;
    }

    while (value > 0 && pos > 2) {
        buf[--pos] = hex[value & 0xF];
        value >>= 4;
    }
    buf[--pos] = 'x';
    buf[--pos] = '0';

    terminal_puts(&buf[pos]);
}

int terminal_get_row(void) {
    return screen_get_row();
}

int terminal_get_col(void) {
    return screen_get_col();
}

void terminal_set_cursor(int row, int col) {
    screen_set_cursor(row, col);
}

void terminal_write_at(int row, int col, char c) {
    screen_write_at(row, col, c);
}