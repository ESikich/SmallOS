#ifndef SMALLOS_GUI_FRAMEWORK_INTERNAL_H
#define SMALLOS_GUI_FRAMEWORK_INTERNAL_H

#include "framework.h"

struct gui_app_context {
    gui_window_t* window;
    void* state;
};

#endif
