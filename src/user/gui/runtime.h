#ifndef SMALLOS_GUI_RUNTIME_H
#define SMALLOS_GUI_RUNTIME_H

typedef struct gui_runtime_options {
    const char* initial_path;
    int diagnostics;
} gui_runtime_options_t;

int gui_runtime_run(const gui_runtime_options_t* options);

#endif
