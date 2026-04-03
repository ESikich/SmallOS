#include "keyboard.h"
#include "ports.h"

/* ------------------------------------------------------------------ */
/* Process-input buffer                                               */
/* ------------------------------------------------------------------ */

#define KB_BUF_SIZE 256

static char kb_buf[KB_BUF_SIZE];
static int  kb_buf_head  = 0;
static int  kb_buf_tail  = 0;
static int  kb_buf_count = 0;

int keyboard_buf_available(void) {
    return kb_buf_count;
}

char keyboard_buf_pop(void) {
    if (kb_buf_count == 0) return 0;
    char c = kb_buf[kb_buf_tail];
    kb_buf_tail = (kb_buf_tail + 1) % KB_BUF_SIZE;
    kb_buf_count--;
    return c;
}

void keyboard_buf_push_char(char c) {
    if (kb_buf_count >= KB_BUF_SIZE) return;
    kb_buf[kb_buf_head] = c;
    kb_buf_head = (kb_buf_head + 1) % KB_BUF_SIZE;
    kb_buf_count++;
}

void keyboard_buf_clear(void) {
    kb_buf_head  = 0;
    kb_buf_tail  = 0;
    kb_buf_count = 0;
}

/* ------------------------------------------------------------------ */
/* Waiting-process slot                                               */
/* ------------------------------------------------------------------ */

/*
 * s_waiting_proc — opaque pointer to the process_t currently parked in
 * PROCESS_STATE_WAITING inside sys_read_impl().
 *
 * Stored as void* to avoid a circular header dependency (keyboard.h is
 * included by process.h).  The only site that inspects the value as a
 * process_t* is process_key_consumer() in process.c, which already has
 * the full type in scope.
 *
 * Written from syscall context (IF=0 during the write) and read from
 * IRQ1 context (IF=0 during the read), so no additional locking is
 * needed on this uniprocessor kernel.
 */
static void* s_waiting_proc = 0;

void keyboard_set_waiting_process(void* proc) {
    s_waiting_proc = proc;
}

void* keyboard_get_waiting_process(void) {
    return s_waiting_proc;
}

/* ------------------------------------------------------------------ */
/* Input consumer                                                     */
/* ------------------------------------------------------------------ */

static keyboard_consumer_fn s_consumer = 0;

void keyboard_set_consumer(keyboard_consumer_fn fn) {
    s_consumer = fn;
}

/* ------------------------------------------------------------------ */

static int shift_down = 0;
static int ctrl_down = 0;
static int alt_down = 0;
static int caps_lock_on = 0;
static int num_lock_on = 0;
static int scroll_lock_on = 0;

static int e0_prefix = 0;
static int e1_prefix = 0;
static unsigned char pause_buf[5];
static int pause_idx = 0;

typedef enum {
    PS_NONE = 0,
    PS_EXPECT_37,
    PS_EXPECT_AA
} ps_state_t;

static ps_state_t ps_state = PS_NONE;

static keycode_t base_keymap[128] = {
    [0x01] = KEY_ESC,
    [0x02] = KEY_1, [0x03] = KEY_2, [0x04] = KEY_3, [0x05] = KEY_4,
    [0x06] = KEY_5, [0x07] = KEY_6, [0x08] = KEY_7, [0x09] = KEY_8,
    [0x0A] = KEY_9, [0x0B] = KEY_0,
    [0x0C] = KEY_MINUS, [0x0D] = KEY_EQUALS,
    [0x0E] = KEY_BACKSPACE,
    [0x0F] = KEY_TAB,

    [0x10] = KEY_Q, [0x11] = KEY_W, [0x12] = KEY_E, [0x13] = KEY_R,
    [0x14] = KEY_T, [0x15] = KEY_Y, [0x16] = KEY_U, [0x17] = KEY_I,
    [0x18] = KEY_O, [0x19] = KEY_P,
    [0x1A] = KEY_LBRACKET, [0x1B] = KEY_RBRACKET,
    [0x1C] = KEY_ENTER,
    [0x1D] = KEY_LCTRL,

    [0x1E] = KEY_A, [0x1F] = KEY_S, [0x20] = KEY_D, [0x21] = KEY_F,
    [0x22] = KEY_G, [0x23] = KEY_H, [0x24] = KEY_J, [0x25] = KEY_K,
    [0x26] = KEY_L,
    [0x27] = KEY_SEMICOLON, [0x28] = KEY_APOSTROPHE, [0x29] = KEY_GRAVE,
    [0x2A] = KEY_LSHIFT,
    [0x2B] = KEY_BACKSLASH,

    [0x2C] = KEY_Z, [0x2D] = KEY_X, [0x2E] = KEY_C, [0x2F] = KEY_V,
    [0x30] = KEY_B, [0x31] = KEY_N, [0x32] = KEY_M,
    [0x33] = KEY_COMMA, [0x34] = KEY_DOT, [0x35] = KEY_SLASH,
    [0x36] = KEY_RSHIFT,

    [0x37] = KEY_KP_STAR,
    [0x38] = KEY_LALT,
    [0x39] = KEY_SPACE,
    [0x3A] = KEY_CAPSLOCK,

    [0x3B] = KEY_F1, [0x3C] = KEY_F2, [0x3D] = KEY_F3, [0x3E] = KEY_F4,
    [0x3F] = KEY_F5, [0x40] = KEY_F6, [0x41] = KEY_F7, [0x42] = KEY_F8,
    [0x43] = KEY_F9, [0x44] = KEY_F10,

    [0x45] = KEY_NUMLOCK,
    [0x46] = KEY_SCROLLLOCK,

    [0x47] = KEY_KP7, [0x48] = KEY_KP8, [0x49] = KEY_KP9,
    [0x4A] = KEY_KP_MINUS,
    [0x4B] = KEY_KP4, [0x4C] = KEY_KP5, [0x4D] = KEY_KP6,
    [0x4E] = KEY_KP_PLUS,
    [0x4F] = KEY_KP1, [0x50] = KEY_KP2, [0x51] = KEY_KP3,
    [0x52] = KEY_KP0,
    [0x53] = KEY_KP_DOT,

    [0x57] = KEY_F11,
    [0x58] = KEY_F12,
};

static keycode_t e0_keymap[128] = {
    [0x1C] = KEY_KP_ENTER,
    [0x1D] = KEY_RCTRL,
    [0x35] = KEY_KP_SLASH,
    [0x38] = KEY_RALT,

    [0x47] = KEY_HOME,
    [0x48] = KEY_UP,
    [0x49] = KEY_PAGEUP,
    [0x4B] = KEY_LEFT,
    [0x4D] = KEY_RIGHT,
    [0x4F] = KEY_END,
    [0x50] = KEY_DOWN,
    [0x51] = KEY_PAGEDOWN,
    [0x52] = KEY_INSERT,
    [0x53] = KEY_DELETE,
};

static char keycode_to_ascii(keycode_t key) {
    int upper = shift_down ^ caps_lock_on;

    switch (key) {
        case KEY_A: return upper ? 'A' : 'a';
        case KEY_B: return upper ? 'B' : 'b';
        case KEY_C: return upper ? 'C' : 'c';
        case KEY_D: return upper ? 'D' : 'd';
        case KEY_E: return upper ? 'E' : 'e';
        case KEY_F: return upper ? 'F' : 'f';
        case KEY_G: return upper ? 'G' : 'g';
        case KEY_H: return upper ? 'H' : 'h';
        case KEY_I: return upper ? 'I' : 'i';
        case KEY_J: return upper ? 'J' : 'j';
        case KEY_K: return upper ? 'K' : 'k';
        case KEY_L: return upper ? 'L' : 'l';
        case KEY_M: return upper ? 'M' : 'm';
        case KEY_N: return upper ? 'N' : 'n';
        case KEY_O: return upper ? 'O' : 'o';
        case KEY_P: return upper ? 'P' : 'p';
        case KEY_Q: return upper ? 'Q' : 'q';
        case KEY_R: return upper ? 'R' : 'r';
        case KEY_S: return upper ? 'S' : 's';
        case KEY_T: return upper ? 'T' : 't';
        case KEY_U: return upper ? 'U' : 'u';
        case KEY_V: return upper ? 'V' : 'v';
        case KEY_W: return upper ? 'W' : 'w';
        case KEY_X: return upper ? 'X' : 'x';
        case KEY_Y: return upper ? 'Y' : 'y';
        case KEY_Z: return upper ? 'Z' : 'z';

        case KEY_1: return shift_down ? '!' : '1';
        case KEY_2: return shift_down ? '@' : '2';
        case KEY_3: return shift_down ? '#' : '3';
        case KEY_4: return shift_down ? '$' : '4';
        case KEY_5: return shift_down ? '%' : '5';
        case KEY_6: return shift_down ? '^' : '6';
        case KEY_7: return shift_down ? '&' : '7';
        case KEY_8: return shift_down ? '*' : '8';
        case KEY_9: return shift_down ? '(' : '9';
        case KEY_0: return shift_down ? ')' : '0';

        case KEY_MINUS: return shift_down ? '_' : '-';
        case KEY_EQUALS: return shift_down ? '+' : '=';
        case KEY_LBRACKET: return shift_down ? '{' : '[';
        case KEY_RBRACKET: return shift_down ? '}' : ']';
        case KEY_BACKSLASH: return shift_down ? '|' : '\\';
        case KEY_SEMICOLON: return shift_down ? ':' : ';';
        case KEY_APOSTROPHE: return shift_down ? '"' : '\'';
        case KEY_GRAVE: return shift_down ? '~' : '`';
        case KEY_COMMA: return shift_down ? '<' : ',';
        case KEY_DOT: return shift_down ? '>' : '.';
        case KEY_SLASH: return shift_down ? '?' : '/';

        case KEY_SPACE: return ' ';
        case KEY_TAB: return '\t';
        case KEY_ENTER: return '\n';
        case KEY_BACKSPACE: return '\b';

        case KEY_KP0: return num_lock_on ? '0' : 0;
        case KEY_KP1: return num_lock_on ? '1' : 0;
        case KEY_KP2: return num_lock_on ? '2' : 0;
        case KEY_KP3: return num_lock_on ? '3' : 0;
        case KEY_KP4: return num_lock_on ? '4' : 0;
        case KEY_KP5: return num_lock_on ? '5' : 0;
        case KEY_KP6: return num_lock_on ? '6' : 0;
        case KEY_KP7: return num_lock_on ? '7' : 0;
        case KEY_KP8: return num_lock_on ? '8' : 0;
        case KEY_KP9: return num_lock_on ? '9' : 0;
        case KEY_KP_DOT: return num_lock_on ? '.' : 0;
        case KEY_KP_PLUS: return '+';
        case KEY_KP_MINUS: return '-';
        case KEY_KP_STAR: return '*';
        case KEY_KP_SLASH: return '/';
        case KEY_KP_ENTER: return '\n';

        default: return 0;
    }
}

static key_event_t decode_scancode(unsigned char scancode) {
    key_event_t ev;
    ev.key = KEY_NONE;
    ev.pressed = 0;
    ev.shift = shift_down;
    ev.ctrl = ctrl_down;
    ev.alt = alt_down;
    ev.caps_lock = caps_lock_on;
    ev.num_lock = num_lock_on;
    ev.scroll_lock = scroll_lock_on;
    ev.ascii = 0;

    if (e1_prefix) {
        pause_buf[pause_idx++] = scancode;

        if (pause_idx == 5) {
            if (pause_buf[0] == 0x1D &&
                pause_buf[1] == 0x45 &&
                pause_buf[2] == 0xE1 &&
                pause_buf[3] == 0x9D &&
                pause_buf[4] == 0xC5) {
                ev.key = KEY_PAUSE;
                ev.pressed = 1;
            }
            e1_prefix = 0;
            pause_idx = 0;
        }

        return ev;
    }

    if (scancode == 0xE1) {
        e1_prefix = 1;
        pause_idx = 0;
        return ev;
    }

    if (scancode == 0xE0) {
        e0_prefix = 1;
        return ev;
    }

    if (e0_prefix) {
        e0_prefix = 0;

        if (ps_state == PS_NONE && scancode == 0x2A) {
            ps_state = PS_EXPECT_37;
            return ev;
        }

        if (ps_state == PS_EXPECT_37 && scancode == 0x37) {
            ev.key = KEY_PRINT_SCREEN;
            ev.pressed = 1;
            ps_state = PS_NONE;
            return ev;
        }

        if (ps_state == PS_NONE && scancode == 0xB7) {
            ps_state = PS_EXPECT_AA;
            return ev;
        }

        if (ps_state == PS_EXPECT_AA && scancode == 0xAA) {
            ev.key = KEY_PRINT_SCREEN;
            ev.pressed = 0;
            ps_state = PS_NONE;
            return ev;
        }

        {
            unsigned char code = scancode & 0x7F;
            int released = (scancode & 0x80) != 0;

            ev.key = e0_keymap[code];
            if (ev.key == KEY_NONE) {
                return ev;
            }
            ev.pressed = !released;
        }
    } else {
        if (ps_state != PS_NONE) {
            ps_state = PS_NONE;
        }

        {
            unsigned char code = scancode & 0x7F;
            int released = (scancode & 0x80) != 0;

            ev.key = base_keymap[code];
            if (ev.key == KEY_NONE) {
                return ev;
            }
            ev.pressed = !released;
        }
    }

    switch (ev.key) {
        case KEY_LSHIFT:
        case KEY_RSHIFT:
            shift_down = ev.pressed;
            break;
        case KEY_LCTRL:
        case KEY_RCTRL:
            ctrl_down = ev.pressed;
            break;
        case KEY_LALT:
        case KEY_RALT:
            alt_down = ev.pressed;
            break;
        case KEY_CAPSLOCK:
            if (ev.pressed) caps_lock_on = !caps_lock_on;
            break;
        case KEY_NUMLOCK:
            if (ev.pressed) num_lock_on = !num_lock_on;
            break;
        case KEY_SCROLLLOCK:
            if (ev.pressed) scroll_lock_on = !scroll_lock_on;
            break;
        default:
            break;
    }

    ev.shift = shift_down;
    ev.ctrl = ctrl_down;
    ev.alt = alt_down;
    ev.caps_lock = caps_lock_on;
    ev.num_lock = num_lock_on;
    ev.scroll_lock = scroll_lock_on;

    if (ev.pressed) {
        ev.ascii = keycode_to_ascii(ev.key);
    }

    return ev;
}

void keyboard_handle_irq(void) {
    unsigned char scancode = inb(0x60);
    key_event_t ev = decode_scancode(scancode);

    if (ev.key == KEY_NONE || !ev.pressed) {
        return;
    }

    if (s_consumer) {
        s_consumer(ev);
    }
}

void keyboard_init(void) {
    shift_down = 0;
    ctrl_down = 0;
    alt_down = 0;
    caps_lock_on = 0;
    num_lock_on = 0;
    scroll_lock_on = 0;
    e0_prefix = 0;
    e1_prefix = 0;
    pause_idx = 0;
    ps_state = PS_NONE;
    s_waiting_proc = 0;
    keyboard_buf_clear();
}