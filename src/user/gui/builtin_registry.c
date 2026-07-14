#include "builtin_registry.h"

#include "about_app.h"
#include "config_app.h"
#include "editor_app.h"
#include "files_app.h"
#include "native_apps.h"
#include "shell_app.h"
#include "system_app.h"

typedef const gui_app_descriptor_t* (*descriptor_fn)(void);

static const descriptor_fn BUILTINS[] = {
    gui_files_app_descriptor,
    gui_editor_app_descriptor,
    gui_shell_app_descriptor,
    gui_viewer_app_descriptor,
    gui_tasks_app_descriptor,
    gui_network_app_descriptor,
    gui_system_app_descriptor,
    gui_config_app_descriptor,
    gui_about_app_descriptor,
};

unsigned int gui_builtin_descriptor_count(void) {
    return sizeof(BUILTINS) / sizeof(BUILTINS[0]);
}

const gui_app_descriptor_t* gui_builtin_descriptor_at(unsigned int index) {
    return index < gui_builtin_descriptor_count() ? BUILTINS[index]() : 0;
}
