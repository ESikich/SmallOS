#ifndef SMALLOS_GUI_FRAMEWORK_H
#define SMALLOS_GUI_FRAMEWORK_H

#include <stdint.h>
#include "app_event.h"
#include "canvas.h"

#define GUI_WINDOW_TITLE_CAPACITY 64

typedef enum gui_app_id {
    GUI_APP_FILES = 1,
    GUI_APP_SYSTEM,
    GUI_APP_CONFIG,
    GUI_APP_ABOUT,
    GUI_APP_SHELL,
    GUI_APP_EDITOR,
    GUI_APP_VIEWER,
    GUI_APP_TASKS,
    GUI_APP_NETWORK,
} gui_app_id_t;

/* Window storage is owned by the window manager. Applications use this opaque
 * handle only through the context and framework operations below. */
typedef struct gui_window gui_window_t;

typedef unsigned int gui_app_event_result_t;

typedef struct gui_app_context {
    gui_window_t* window;
    void* state;
} gui_app_context_t;

typedef struct gui_app_descriptor {
    const char* title;
    unsigned int state_size;
    int default_width;
    int default_height;
    int min_width;
    int min_height;
    uint32_t tick_interval;
    void (*open)(gui_app_context_t* context, const char* argument);
    void (*close)(gui_app_context_t* context);
    void (*draw)(gfx_surface_t* surface, gui_app_context_t* context,
                 int mx, int my);
    unsigned int (*event)(gui_app_context_t* context,
                          const gui_app_event_t* event);
    gui_app_id_t id;
    int background_ticks;
    const char* icon;
    const char* launcher_label;
    int launcher_order;
    int show_in_start;
} gui_app_descriptor_t;

typedef enum gui_file_filter {
    GUI_FILE_FILTER_ANY = 0,
    GUI_FILE_FILTER_TEXT,
    GUI_FILE_FILTER_BMP,
} gui_file_filter_t;

typedef enum gui_file_request_mode {
    GUI_FILE_REQUEST_OPEN = 1,
    GUI_FILE_REQUEST_SAVE,
} gui_file_request_mode_t;

gui_window_t* gui_open_app(gui_app_id_t id, const char* argument);
void gui_window_request_close(gui_window_t* window);
void gui_preferences_save(void);
void gui_window_set_title(gui_window_t* window, const char* title);
void gui_window_invalidate_local(gui_window_t* window,
                                 int x, int y, int width, int height);

/* The registry is the single source of launcher and descriptor metadata. */
void gui_app_registry_reset(void);
int gui_app_registry_add(const gui_app_descriptor_t* descriptor);
const gui_app_descriptor_t* gui_app_registry_find(gui_app_id_t id);
unsigned int gui_app_registry_count(void);
const gui_app_descriptor_t* gui_app_registry_at(unsigned int index);
unsigned int gui_app_registry_launcher_count(void);
const gui_app_descriptor_t* gui_app_registry_launcher_at(unsigned int index);

void* gui_app_state(gui_app_context_t* context);
gui_window_t* gui_app_window(gui_app_context_t* context);
void gui_app_set_title(gui_app_context_t* context, const char* title);
void gui_app_invalidate(gui_app_context_t* context,
                        int x, int y, int width, int height);
void gui_app_request_close(gui_app_context_t* context);
gui_window_t* gui_app_open(gui_app_context_t* context,
                           gui_app_id_t id, const char* argument);
int gui_app_open_file_picker(gui_app_context_t* context,
                             gui_file_request_mode_t mode,
                             gui_file_filter_t filter,
                             const char* initial_path);

#endif
