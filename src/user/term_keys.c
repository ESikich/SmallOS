#include "term_keys.h"

#include "poll.h"
#include "user_syscall.h"

int term_key_available(void) {
    struct pollfd pfd;

    pfd.fd = 0;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return sys_poll(&pfd, 1u, 0) == 1 && (pfd.revents & POLLIN) != 0;
}

int term_key_read_raw(int block) {
    char c;

    if (!block && !term_key_available()) {
        return TERM_KEY_NONE;
    }

    while (1) {
        if (sys_read_raw(&c, 1u) == 1) {
            return (unsigned char)c;
        }
        if (!block) {
            return TERM_KEY_NONE;
        }
        sys_sleep(1);
    }
}

static int read_csi_key(void) {
    int c = term_key_read_raw(0);

    switch (c) {
        case 'A': return TERM_KEY_UP;
        case 'B': return TERM_KEY_DOWN;
        case 'C': return TERM_KEY_RIGHT;
        case 'D': return TERM_KEY_LEFT;
        case 'H': return TERM_KEY_HOME;
        case 'F': return TERM_KEY_END;
        case '2':
            (void)term_key_read_raw(0);
            return TERM_KEY_INSERT;
        case '3':
            (void)term_key_read_raw(0);
            return TERM_KEY_DELETE;
        case '5':
            (void)term_key_read_raw(0);
            return TERM_KEY_PAGE_UP;
        case '6':
            (void)term_key_read_raw(0);
            return TERM_KEY_PAGE_DOWN;
        default:
            return TERM_KEY_ESC;
    }
}

static int read_ss3_key(void) {
    int c = term_key_read_raw(0);

    switch (c) {
        case 'P': return TERM_KEY_F1;
        case 'Q': return TERM_KEY_F2;
        case 'R': return TERM_KEY_F3;
        case 'S': return TERM_KEY_F4;
        default: return TERM_KEY_ESC;
    }
}

int term_key_read(int block) {
    int c = term_key_read_raw(block);

    switch (c) {
        case TERM_KEY_NONE: return TERM_KEY_NONE;
        case '\n':
        case '\r':
            return TERM_KEY_ENTER;
        case '\b':
        case 127:
            return TERM_KEY_BACKSPACE;
        case '\t':
            return TERM_KEY_TAB;
        case 1:
            return TERM_KEY_HOME;
        case 3:
            return TERM_KEY_CTRL_C;
        case 4:
            return TERM_KEY_CTRL_D;
        case 5:
            return TERM_KEY_END;
        case 21:
            return TERM_KEY_CTRL_U;
        case 0x1b: {
            int next = term_key_read_raw(0);

            if (next == '[') {
                return read_csi_key();
            }
            if (next == 'O') {
                return read_ss3_key();
            }
            return TERM_KEY_ESC;
        }
        default:
            return c;
    }
}

void term_key_drain(void) {
    int idle_ticks = 0;

    while (idle_ticks < 3) {
        if (!term_key_available()) {
            idle_ticks++;
            sys_sleep(1);
            continue;
        }
        if (term_key_read_raw(0) == TERM_KEY_NONE) {
            break;
        }
        idle_ticks = 0;
    }
}
