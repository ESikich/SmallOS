#ifndef SMALLOS_GUI_APP_EVENT_H
#define SMALLOS_GUI_APP_EVENT_H

#include <stdint.h>

typedef enum {
    GUI_APP_EVENT_KEY = 1,
    GUI_APP_EVENT_POINTER_DOWN,
    GUI_APP_EVENT_POINTER_UP,
    GUI_APP_EVENT_POINTER_MOVE,
    GUI_APP_EVENT_WHEEL,
    GUI_APP_EVENT_RESIZE,
    GUI_APP_EVENT_TICK,
    GUI_APP_EVENT_CLOSE_REQUEST,
    GUI_APP_EVENT_FOCUS_GAINED,
    GUI_APP_EVENT_FOCUS_LOST,
    GUI_APP_EVENT_FILE_SELECTED,
    GUI_APP_EVENT_FILE_CANCELLED,
} gui_app_event_type_t;

typedef struct {
    gui_app_event_type_t type;
    int x;
    int y;
    int width;
    int height;
    int wheel;
    unsigned int key;
    unsigned int ascii;
    unsigned int modifiers;
    unsigned int buttons;
    uint32_t ticks;
    const char* path;
    unsigned int file_filter;
} gui_app_event_t;

enum {
    GUI_APP_RESULT_NONE = 0,
    GUI_APP_RESULT_HANDLED = 1u << 0,
    GUI_APP_RESULT_REDRAW = 1u << 1,
    GUI_APP_RESULT_CLOSE = 1u << 2,
    GUI_APP_RESULT_KEEP_OPEN = 1u << 3,
};

#endif
