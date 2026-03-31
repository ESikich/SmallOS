#ifndef TERMINAL_H
#define TERMINAL_H

void terminal_init(void);
void terminal_clear(void);
void terminal_putc(char c);
void terminal_puts(const char* s);
void terminal_put_uint(unsigned int value);   /* decimal, no newline */
void terminal_put_hex(unsigned int value);    /* "0x" + uppercase hex, no newline */

int  terminal_get_row(void);
int  terminal_get_col(void);
void terminal_set_cursor(int row, int col);
void terminal_write_at(int row, int col, char c);

#endif