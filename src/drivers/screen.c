#include "screen.h"
#include "ports.h"

#define VGA_TEXT_COLS 80
#define VGA_TEXT_ROWS 25

static volatile unsigned short* const VGA_MEMORY = (unsigned short*)0xB8000;
static int row = 0;
static int col = 0;
static unsigned char color = 0x0F;

static void move_hw_cursor(void) {
    unsigned short pos = (unsigned short)(row * VGA_TEXT_COLS + col);

    outb(0x3D4, 0x0F);
    outb(0x3D5, (unsigned char)(pos & 0xFF));

    outb(0x3D4, 0x0E);
    outb(0x3D5, (unsigned char)((pos >> 8) & 0xFF));
}

static void scroll(void) {
    for (int y = 1; y < VGA_TEXT_ROWS; y++) {
        for (int x = 0; x < VGA_TEXT_COLS; x++) {
            VGA_MEMORY[(y - 1) * VGA_TEXT_COLS + x] =
                VGA_MEMORY[y * VGA_TEXT_COLS + x];
        }
    }

    for (int x = 0; x < VGA_TEXT_COLS; x++) {
        VGA_MEMORY[(VGA_TEXT_ROWS - 1) * VGA_TEXT_COLS + x] =
            ((unsigned short)color << 8) | ' ';
    }

    row = VGA_TEXT_ROWS - 1;
    col = 0;
}

void screen_clear(void) {
    for (int y = 0; y < VGA_TEXT_ROWS; y++) {
        for (int x = 0; x < VGA_TEXT_COLS; x++) {
            VGA_MEMORY[y * VGA_TEXT_COLS + x] =
                ((unsigned short)color << 8) | ' ';
        }
    }

    row = 0;
    col = 0;
    move_hw_cursor();
}

void screen_putc(char c) {
    if (c == '\n') {
        col = 0;
        row++;
    } else if (c == '\r') {
        /* Keep CRLF text files from rendering the VGA control glyph for CR. */
        col = 0;
    } else if (c == '\b') {
        if (col > 0) {
            col--;
            VGA_MEMORY[row * VGA_TEXT_COLS + col] =
                ((unsigned short)color << 8) | ' ';
        }
    } else {
        VGA_MEMORY[row * VGA_TEXT_COLS + col] =
            ((unsigned short)color << 8) | (unsigned char)c;
        col++;

        if (col >= VGA_TEXT_COLS) {
            col = 0;
            row++;
        }
    }

    if (row >= VGA_TEXT_ROWS) {
        scroll();
    }

    move_hw_cursor();
}

void screen_puts(const char* s) {
    for (int i = 0; s[i] != '\0'; i++) {
        screen_putc(s[i]);
    }
}

int screen_get_row(void) {
    return row;
}

int screen_get_col(void) {
    return col;
}

int screen_rows(void) {
    return VGA_TEXT_ROWS;
}

int screen_cols(void) {
    return VGA_TEXT_COLS;
}

void screen_set_cursor(int new_row, int new_col) {
    if (new_row < 0) new_row = 0;
    if (new_row >= VGA_TEXT_ROWS) new_row = VGA_TEXT_ROWS - 1;
    if (new_col < 0) new_col = 0;
    if (new_col >= VGA_TEXT_COLS) new_col = VGA_TEXT_COLS - 1;

    row = new_row;
    col = new_col;
    move_hw_cursor();
}

void screen_write_at(int r, int c, char ch) {
    if (r < 0 || r >= VGA_TEXT_ROWS || c < 0 || c >= VGA_TEXT_COLS) {
        return;
    }

    VGA_MEMORY[r * VGA_TEXT_COLS + c] =
        ((unsigned short)color << 8) | (unsigned char)ch;
}

const terminal_backend_t vga_text_backend = {
    .clear = screen_clear,
    .putc = screen_putc,
    .rows = screen_rows,
    .cols = screen_cols,
    .row = screen_get_row,
    .col = screen_get_col,
    .set_cursor = screen_set_cursor,
    .write_at = screen_write_at,
};
