#include "screen.h"
#include "ports.h"

static volatile unsigned short* const VGA_MEMORY = (unsigned short*)0xB8000;
static int row = 0;
static int col = 0;
static unsigned char color = 0x0F;

static void move_hw_cursor(void) {
    unsigned short pos = (unsigned short)(row * 80 + col);

    outb(0x3D4, 0x0F);
    outb(0x3D5, (unsigned char)(pos & 0xFF));

    outb(0x3D4, 0x0E);
    outb(0x3D5, (unsigned char)((pos >> 8) & 0xFF));
}

static void scroll(void) {
    for (int y = 1; y < 25; y++) {
        for (int x = 0; x < 80; x++) {
            VGA_MEMORY[(y - 1) * 80 + x] = VGA_MEMORY[y * 80 + x];
        }
    }

    for (int x = 0; x < 80; x++) {
        VGA_MEMORY[24 * 80 + x] = ((unsigned short)color << 8) | ' ';
    }

    row = 24;
    col = 0;
}

void screen_clear(void) {
    for (int y = 0; y < 25; y++) {
        for (int x = 0; x < 80; x++) {
            VGA_MEMORY[y * 80 + x] = ((unsigned short)color << 8) | ' ';
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
    } else if (c == '\b') {
        if (col > 0) {
            col--;
            VGA_MEMORY[row * 80 + col] = ((unsigned short)color << 8) | ' ';
        }
    } else {
        VGA_MEMORY[row * 80 + col] = ((unsigned short)color << 8) | (unsigned char)c;
        col++;

        if (col >= 80) {
            col = 0;
            row++;
        }
    }

    if (row >= 25) {
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

void screen_set_cursor(int new_row, int new_col) {
    if (new_row < 0) new_row = 0;
    if (new_row > 24) new_row = 24;
    if (new_col < 0) new_col = 0;
    if (new_col > 79) new_col = 79;

    row = new_row;
    col = new_col;
    move_hw_cursor();
}

void screen_write_at(int r, int c, char ch) {
    if (r < 0 || r > 24 || c < 0 || c > 79) {
        return;
    }

    VGA_MEMORY[r * 80 + c] = ((unsigned short)color << 8) | (unsigned char)ch;
}