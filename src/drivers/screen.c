#include "screen.h"
#include "ports.h"

static volatile unsigned short* const VGA_MEMORY = (unsigned short*)0xB8000;
static int row = 0;
static int col = 0;
static unsigned char color = 0x0F;
static int esc_state = 0;
static int csi_args[4];
static int csi_arg_count = 0;
static int csi_value = 0;
static int csi_has_value = 0;
static int csi_private = 0;

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

static void clear_line_from_cursor(void) {
    for (int x = col; x < 80; x++) {
        VGA_MEMORY[row * 80 + x] = ((unsigned short)color << 8) | ' ';
    }
}

static void csi_push_arg(void) {
    if (csi_arg_count >= 4) return;
    csi_args[csi_arg_count++] = csi_has_value ? csi_value : 0;
    csi_value = 0;
    csi_has_value = 0;
}

static int csi_arg_or(int index, int fallback) {
    if (index >= csi_arg_count || csi_args[index] == 0) {
        return fallback;
    }
    return csi_args[index];
}

static void csi_dispatch(char cmd) {
    csi_push_arg();

    switch (cmd) {
        case 'A': {
            row -= csi_arg_or(0, 1);
            if (row < 0) row = 0;
            break;
        }
        case 'B': {
            row += csi_arg_or(0, 1);
            if (row > 24) row = 24;
            break;
        }
        case 'C': {
            col += csi_arg_or(0, 1);
            if (col > 79) col = 79;
            break;
        }
        case 'D': {
            col -= csi_arg_or(0, 1);
            if (col < 0) col = 0;
            break;
        }
        case 'H':
        case 'f': {
            row = csi_arg_or(0, 1) - 1;
            col = csi_arg_or(1, 1) - 1;
            if (row < 0) row = 0;
            if (row > 24) row = 24;
            if (col < 0) col = 0;
            if (col > 79) col = 79;
            break;
        }
        case 'J': {
            if (csi_arg_or(0, 0) == 2) {
                screen_clear();
                return;
            }
            break;
        }
        case 'K': {
            clear_line_from_cursor();
            break;
        }
        case 'l':
        case 'h':
            /* Cursor visibility and private modes are ignored on VGA text. */
            (void)csi_private;
            break;
        default:
            break;
    }

    move_hw_cursor();
}

static int screen_handle_escape(char c) {
    if (esc_state == 0) {
        if ((unsigned char)c == 27) {
            esc_state = 1;
            return 1;
        }
        return 0;
    }

    if (esc_state == 1) {
        if (c == '[') {
            esc_state = 2;
            csi_arg_count = 0;
            csi_value = 0;
            csi_has_value = 0;
            csi_private = 0;
            for (int i = 0; i < 4; i++) csi_args[i] = 0;
            return 1;
        }
        esc_state = 0;
        return 1;
    }

    if (esc_state == 2) {
        if (c == '?') {
            csi_private = 1;
            return 1;
        }
        if (c >= '0' && c <= '9') {
            csi_value = csi_value * 10 + (c - '0');
            csi_has_value = 1;
            return 1;
        }
        if (c == ';') {
            csi_push_arg();
            return 1;
        }
        csi_dispatch(c);
        esc_state = 0;
        return 1;
    }

    esc_state = 0;
    return 0;
}

void screen_putc(char c) {
    if (screen_handle_escape(c)) {
        return;
    }

    if (c == '\n') {
        col = 0;
        row++;
    } else if (c == '\r') {
        /* Keep CRLF text files from rendering the VGA control glyph for CR. */
        col = 0;
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
