#ifndef TERMINAL_H
#define TERMINAL_H

typedef struct {
    void (*clear)(void);
    void (*putc)(char c);
    void (*begin_update)(void);
    void (*end_update)(void);
    int  (*rows)(void);
    int  (*cols)(void);
    int  (*row)(void);
    int  (*col)(void);
    void (*set_cursor)(int row, int col);
    void (*write_at)(int row, int col, char c);
} terminal_backend_t;

typedef void (*terminal_output_hook_t)(char c);
typedef const char* (*terminal_line_prefix_hook_t)(void);

void terminal_init(void);
void terminal_set_backend(const terminal_backend_t* backend);
void terminal_set_display_enabled(int enabled);
void terminal_set_output_hook(terminal_output_hook_t hook);
void terminal_set_line_prefix_hook(terminal_line_prefix_hook_t hook);
void terminal_clear(void);
void terminal_putc(char c);
void terminal_write(const char* s, unsigned int len);
void terminal_puts(const char* s);
void terminal_put_uint(unsigned int value);   /* decimal, no newline */
void terminal_put_hex(unsigned int value);    /* "0x" + uppercase hex, no newline */

int  terminal_rows(void);
int  terminal_cols(void);
int  terminal_get_row(void);
int  terminal_get_col(void);
void terminal_set_cursor(int row, int col);
void terminal_write_at(int row, int col, char c);

#endif
