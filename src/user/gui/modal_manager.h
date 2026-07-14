#ifndef SMALLOS_GUI_MODAL_MANAGER_H
#define SMALLOS_GUI_MODAL_MANAGER_H

#include "framework.h"

typedef void (*gui_modal_dispatch_fn)(gui_window_t* owner,
                                      const gui_app_event_t* event,
                                      void* opaque);
typedef void (*gui_modal_invalidate_fn)(void* opaque);
typedef void (*gui_modal_notice_fn)(const char* text, void* opaque);
typedef int (*gui_modal_owner_active_fn)(gui_window_t* owner, void* opaque);

typedef struct gui_modal_services {
    gui_modal_dispatch_fn dispatch;
    gui_modal_invalidate_fn invalidate;
    gui_modal_notice_fn notice;
    gui_modal_owner_active_fn owner_active;
    void* opaque;
} gui_modal_services_t;

void gui_modal_init(int screen_width, int screen_height,
                    const gui_modal_services_t* services);
void gui_modal_resize(int screen_width, int screen_height);
int gui_modal_active(void);
int gui_modal_open_picker(gui_window_t* owner,
                          gui_file_request_mode_t mode,
                          gui_file_filter_t filter,
                          const char* initial_path);
int gui_modal_open_dialog(gui_window_t* owner,
                          const gui_dialog_request_t* request);
void gui_modal_dismiss_owner(gui_window_t* owner);
int gui_modal_event(const gui_app_event_t* event);
void gui_modal_draw(gfx_surface_t* surface);
gui_rect_t gui_modal_bounds(void);

#endif
