#ifndef KEYBOARD_H
#define KEYBOARD_H

#include <stddef.h>

typedef enum {
    KEY_NONE = 0,

    KEY_ESC,
    KEY_1, KEY_2, KEY_3, KEY_4, KEY_5, KEY_6, KEY_7, KEY_8, KEY_9, KEY_0,
    KEY_MINUS,
    KEY_EQUALS,
    KEY_BACKSPACE,
    KEY_TAB,

    KEY_Q, KEY_W, KEY_E, KEY_R, KEY_T, KEY_Y, KEY_U, KEY_I, KEY_O, KEY_P,
    KEY_LBRACKET,
    KEY_RBRACKET,
    KEY_ENTER,
    KEY_LCTRL,

    KEY_A, KEY_S, KEY_D, KEY_F, KEY_G, KEY_H, KEY_J, KEY_K, KEY_L,
    KEY_SEMICOLON,
    KEY_APOSTROPHE,
    KEY_GRAVE,
    KEY_LSHIFT,
    KEY_BACKSLASH,

    KEY_Z, KEY_X, KEY_C, KEY_V, KEY_B, KEY_N, KEY_M,
    KEY_COMMA,
    KEY_DOT,
    KEY_SLASH,
    KEY_RSHIFT,

    KEY_KP_STAR,
    KEY_LALT,
    KEY_SPACE,
    KEY_CAPSLOCK,

    KEY_F1, KEY_F2, KEY_F3, KEY_F4, KEY_F5, KEY_F6,
    KEY_F7, KEY_F8, KEY_F9, KEY_F10,

    KEY_NUMLOCK,
    KEY_SCROLLLOCK,

    KEY_KP7, KEY_KP8, KEY_KP9,
    KEY_KP_MINUS,
    KEY_KP4, KEY_KP5, KEY_KP6,
    KEY_KP_PLUS,
    KEY_KP1, KEY_KP2, KEY_KP3,
    KEY_KP0,
    KEY_KP_DOT,

    KEY_F11,
    KEY_F12,

    KEY_RCTRL,
    KEY_RALT,

    KEY_HOME,
    KEY_UP,
    KEY_PAGEUP,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_END,
    KEY_DOWN,
    KEY_PAGEDOWN,
    KEY_INSERT,
    KEY_DELETE,

    KEY_KP_ENTER,
    KEY_KP_SLASH,
    KEY_PRINT_SCREEN,
    KEY_PAUSE,
} keycode_t;

typedef struct {
    keycode_t key;
    int pressed;
    int shift;
    int ctrl;
    int alt;
    int caps_lock;
    int num_lock;
    int scroll_lock;
    char ascii;
} key_event_t;

/*
 * keyboard_consumer_fn — called from keyboard_handle_irq() for every
 * key-press event after scancode decoding.  The registered consumer
 * receives the full key_event_t and decides what to do with it.
 *
 * Runs in IRQ context — must not block or sleep.
 */
typedef void (*keyboard_consumer_fn)(key_event_t ev);

void keyboard_init(void);
void keyboard_handle_irq(void);

/*
 * keyboard_set_consumer(fn)
 *
 * Register the active input consumer.  Pass NULL to unregister.
 * The keyboard driver calls fn for every key-press event and makes
 * no routing decisions of its own.
 *
 * Ownership transfers are:
 *   shell_init()            — registers the shell consumer
 *   process_set_foreground  — registers a process consumer (or restores shell)
 */
void keyboard_set_consumer(keyboard_consumer_fn fn);

/*
 * Raw ASCII ring buffer — written by the process-mode consumer,
 * drained by SYS_READ in syscall.c.
 */
int  keyboard_buf_available(void);
char keyboard_buf_pop(void);
void keyboard_buf_push_char(char c);

#endif