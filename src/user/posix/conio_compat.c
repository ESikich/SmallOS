#include "conio.h"

#include "smallos_input.h"
#include "stdio.h"
#include "unistd.h"

#define CONIO_KEYBUF_SIZE 16u

static unsigned int s_keybuf[CONIO_KEYBUF_SIZE];
static unsigned int s_key_head;
static unsigned int s_key_tail;
static unsigned int s_key_count;

static void conio_key_push(unsigned int value) {
    if (!value) {
        return;
    }
    if (s_key_count >= CONIO_KEYBUF_SIZE) {
        s_key_tail = (s_key_tail + 1u) % CONIO_KEYBUF_SIZE;
        s_key_count--;
    }
    s_keybuf[s_key_head] = value;
    s_key_head = (s_key_head + 1u) % CONIO_KEYBUF_SIZE;
    s_key_count++;
}

static unsigned int conio_key_pop(void) {
    unsigned int value;

    if (!s_key_count) {
        return 0;
    }
    value = s_keybuf[s_key_tail];
    s_key_tail = (s_key_tail + 1u) % CONIO_KEYBUF_SIZE;
    s_key_count--;
    return value;
}

static unsigned int conio_key_peek(void) {
    return s_key_count ? s_keybuf[s_key_tail] : 0u;
}

static unsigned int conio_event_to_bios_key(const sys_input_event_t* ev) {
    unsigned int ascii;
    unsigned int scan;

    if (!ev || ev->type != SYS_INPUT_TYPE_KEY ||
        (ev->flags & SYS_INPUT_KEY_PRESSED) == 0u) {
        return 0;
    }

    ascii = ev->ascii & 0xffu;
    scan = ev->key & 0xffu;
    return (scan << 8) | ascii;
}

static int conio_pump(int block) {
    sys_input_event_t ev[8];
    int flags = block ? 0 : SYS_INPUT_FLAG_NONBLOCK;
    int n = smallos_input_read(ev, sizeof(ev) / sizeof(ev[0]),
                               (uint32_t)flags);

    if (n <= 0) {
        return 0;
    }
    for (int i = 0; i < n; i++) {
        conio_key_push(conio_event_to_bios_key(&ev[i]));
    }
    return s_key_count ? 1 : 0;
}

int kbhit(void) {
    if (s_key_count) {
        return 1;
    }
    return conio_pump(0);
}

int getch(void) {
    unsigned int value;

    while (!s_key_count) {
        (void)conio_pump(1);
    }

    value = conio_key_pop();
    if ((value & 0xffu) != 0u) {
        return (int)(value & 0xffu);
    }
    return (int)((value >> 8) & 0xffu);
}

int bioskey(int command) {
    if (command == 1) {
        if (!s_key_count) {
            (void)conio_pump(0);
        }
        return (int)conio_key_peek();
    }
    if (command == 2) {
        return 0;
    }

    while (!s_key_count) {
        (void)conio_pump(1);
    }
    return (int)conio_key_pop();
}

void gotoxy(int x, int y) {
    if (x < 1) {
        x = 1;
    }
    if (y < 1) {
        y = 1;
    }
    printf("\033[%d;%dH", y, x);
}

void clrscr(void) {
    (void)write(1, "\033[2J\033[H", 7u);
}
