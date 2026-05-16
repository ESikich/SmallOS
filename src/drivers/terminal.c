#include "terminal.h"
#include "screen.h"
#include "serial.h"
#include "unicode.h"

static const terminal_backend_t* active_backend = &vga_text_backend;
static int esc_state = 0;
static int csi_args[4];
static int csi_arg_count = 0;
static int csi_value = 0;
static int csi_has_value = 0;
static int csi_private = 0;
static utf8_decoder_t utf8_decoder;
static terminal_output_hook_t output_hook = 0;

static int clamp_int(int value, int min, int max) {
    if (value < min) return min;
    if (value > max) return max;
    return value;
}

static int backend_rows(void) {
    return active_backend && active_backend->rows ? active_backend->rows() : 25;
}

static int backend_cols(void) {
    return active_backend && active_backend->cols ? active_backend->cols() : 80;
}

static int backend_row(void) {
    return active_backend && active_backend->row ? active_backend->row() : 0;
}

static int backend_col(void) {
    return active_backend && active_backend->col ? active_backend->col() : 0;
}

static void backend_set_cursor(int row, int col) {
    if (active_backend && active_backend->set_cursor) {
        active_backend->set_cursor(row, col);
    }
}

static void clear_line_from_cursor(void) {
    int row = backend_row();
    int col = backend_col();
    int cols = backend_cols();

    for (int x = col; x < cols; x++) {
        terminal_write_at(row, x, ' ');
    }
    backend_set_cursor(row, col);
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
    int row = backend_row();
    int col = backend_col();
    int rows = backend_rows();
    int cols = backend_cols();

    csi_push_arg();

    switch (cmd) {
        case 'A':
            row -= csi_arg_or(0, 1);
            break;
        case 'B':
            row += csi_arg_or(0, 1);
            break;
        case 'C':
            col += csi_arg_or(0, 1);
            break;
        case 'D':
            col -= csi_arg_or(0, 1);
            break;
        case 'H':
        case 'f':
            row = csi_arg_or(0, 1) - 1;
            col = csi_arg_or(1, 1) - 1;
            break;
        case 'J':
            if (csi_arg_or(0, 0) == 2) {
                terminal_clear();
                return;
            }
            break;
        case 'K':
            clear_line_from_cursor();
            return;
        case 'l':
        case 'h':
            /* Cursor visibility and private modes are backend-local for now. */
            (void)csi_private;
            return;
        default:
            return;
    }

    backend_set_cursor(clamp_int(row, 0, rows - 1),
                       clamp_int(col, 0, cols - 1));
}

static int terminal_handle_escape(char c) {
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

static void terminal_emit_codepoint(unsigned int cp) {
    if (active_backend && active_backend->putc) {
        active_backend->putc((char)unicode_to_cp437(cp));
    }
}

static void terminal_emit_utf8_byte(unsigned char b) {
    for (;;) {
        unsigned int cp = 0;
        utf8_decode_result_t result = utf8_decoder_feed(&utf8_decoder, b, &cp);

        if (result == UTF8_DECODE_WAIT) {
            return;
        }

        terminal_emit_codepoint(cp);
        if (result != UTF8_DECODE_REJECT_RETRY) {
            return;
        }
    }
}

void terminal_init(void) {
#ifdef SMALLOS_SERIAL_CONSOLE
    serial_init();
#endif
    terminal_clear();
}

void terminal_set_backend(const terminal_backend_t* backend) {
    if (!backend) {
        return;
    }

    active_backend = backend;
    esc_state = 0;
    utf8_decoder_reset(&utf8_decoder);
}

void terminal_set_output_hook(terminal_output_hook_t hook) {
    output_hook = hook;
}

void terminal_clear(void) {
    if (active_backend && active_backend->clear) {
        active_backend->clear();
    }
}

void terminal_putc(char c) {
    unsigned char b = (unsigned char)c;

    if (terminal_handle_escape(c)) {
        utf8_decoder_reset(&utf8_decoder);
    } else {
        terminal_emit_utf8_byte(b);
    }
#ifdef SMALLOS_SERIAL_CONSOLE
    serial_putc(c);
#endif
    if (output_hook) {
        output_hook(c);
    }
}

void terminal_write(const char* s, unsigned int len) {
    if (!s || len == 0u) {
        return;
    }

    if (active_backend && active_backend->begin_update) {
        active_backend->begin_update();
    }

    for (unsigned int i = 0; i < len; i++) {
        unsigned char b = (unsigned char)s[i];

        if (terminal_handle_escape((char)b)) {
            utf8_decoder_reset(&utf8_decoder);
        } else {
            terminal_emit_utf8_byte(b);
        }
#ifdef SMALLOS_SERIAL_CONSOLE
        serial_putc((char)b);
#endif
        if (output_hook) {
            output_hook((char)b);
        }
    }

    if (active_backend && active_backend->end_update) {
        active_backend->end_update();
    }
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
    return backend_row();
}

int terminal_get_col(void) {
    return backend_col();
}

int terminal_rows(void) {
    return backend_rows();
}

int terminal_cols(void) {
    return backend_cols();
}

void terminal_set_cursor(int row, int col) {
    backend_set_cursor(row, col);
}

void terminal_write_at(int row, int col, char c) {
    if (active_backend && active_backend->write_at) {
        active_backend->write_at(row, col, c);
    }
}
