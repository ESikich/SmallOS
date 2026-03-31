#ifndef SCREEN_H
#define SCREEN_H

void screen_clear(void);
void screen_putc(char c);
void screen_puts(const char* s);

int screen_get_row(void);
int screen_get_col(void);
void screen_set_cursor(int row, int col);
void screen_write_at(int row, int col, char c);

#endif