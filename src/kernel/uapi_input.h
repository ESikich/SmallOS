#ifndef UAPI_INPUT_H
#define UAPI_INPUT_H

#define SYS_MOUSE_BUTTON_LEFT   0x01u
#define SYS_MOUSE_BUTTON_RIGHT  0x02u
#define SYS_MOUSE_BUTTON_MIDDLE 0x04u

#define SYS_INPUT_TYPE_KEY   1u
#define SYS_INPUT_TYPE_MOUSE 2u

#define SYS_INPUT_FLAG_NONBLOCK 0x01u

#define SYS_INPUT_KEY_PRESSED     0x0001u
#define SYS_INPUT_KEY_SHIFT       0x0002u
#define SYS_INPUT_KEY_CTRL        0x0004u
#define SYS_INPUT_KEY_ALT         0x0008u
#define SYS_INPUT_KEY_CAPS_LOCK   0x0010u
#define SYS_INPUT_KEY_NUM_LOCK    0x0020u
#define SYS_INPUT_KEY_SCROLL_LOCK 0x0040u

typedef struct sys_mouse_state {
    int dx;
    int dy;
    unsigned int buttons;
    unsigned int sequence;
} sys_mouse_state_t;

typedef struct sys_input_event {
    unsigned int type;
    unsigned int flags;
    unsigned int ticks;
    unsigned int sequence;
    unsigned int key;
    unsigned int ascii;
    int dx;
    int dy;
    unsigned int buttons;
    unsigned int button_changes;
} sys_input_event_t;

#endif /* UAPI_INPUT_H */
