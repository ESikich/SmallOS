#ifndef TERM_KEYS_H
#define TERM_KEYS_H

typedef enum term_key {
    TERM_KEY_NONE = 0,
    TERM_KEY_ESC = 256,
    TERM_KEY_ENTER,
    TERM_KEY_BACKSPACE,
    TERM_KEY_TAB,
    TERM_KEY_DELETE,
    TERM_KEY_INSERT,
    TERM_KEY_LEFT,
    TERM_KEY_RIGHT,
    TERM_KEY_UP,
    TERM_KEY_DOWN,
    TERM_KEY_HOME,
    TERM_KEY_END,
    TERM_KEY_PAGE_UP,
    TERM_KEY_PAGE_DOWN,
    TERM_KEY_F1,
    TERM_KEY_F2,
    TERM_KEY_F3,
    TERM_KEY_F4,
    TERM_KEY_CTRL_C,
    TERM_KEY_CTRL_D,
    TERM_KEY_CTRL_U
} term_key_t;

int term_key_available(void);
int term_key_read(int block);
int term_key_read_raw(int block);
void term_key_drain(void);

#endif /* TERM_KEYS_H */
