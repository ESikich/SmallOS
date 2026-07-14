#ifndef SMALLOS_GUI_FRAMEWORK_INTERNAL_H
#define SMALLOS_GUI_FRAMEWORK_INTERNAL_H

#include "framework.h"

struct gui_window {
    int active;
    int minimized;
    int maximized;
    gui_app_id_t type;
    int x, y, w, h;
    int restore_x, restore_y, restore_w, restore_h;
    char title[GUI_WINDOW_TITLE_CAPACITY];
    void* state;
    int focused_widget;
    int pressed_widget;
    uint32_t next_tick;
};

#endif
