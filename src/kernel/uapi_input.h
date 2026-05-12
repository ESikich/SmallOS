#ifndef UAPI_INPUT_H
#define UAPI_INPUT_H

#define SYS_MOUSE_BUTTON_LEFT   0x01u
#define SYS_MOUSE_BUTTON_RIGHT  0x02u
#define SYS_MOUSE_BUTTON_MIDDLE 0x04u

typedef struct sys_mouse_state {
    int dx;
    int dy;
    unsigned int buttons;
    unsigned int sequence;
} sys_mouse_state_t;

#endif /* UAPI_INPUT_H */
