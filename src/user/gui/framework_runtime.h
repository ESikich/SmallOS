#ifndef SMALLOS_GUI_FRAMEWORK_RUNTIME_H
#define SMALLOS_GUI_FRAMEWORK_RUNTIME_H

#include "framework.h"
#include "poll.h"

typedef struct gui_framework_services {
    void (*invalidate_rect)(gui_rect_t rect);
    void (*invalidate_full)(void);
    void (*invalidate_taskbar)(void);
    void (*notice)(const char* text, uint32_t until);
    int (*compatibility_handoff)(const char* path);
} gui_framework_services_t;

void gui_framework_runtime_init(int width, int height,
                                const gui_framework_services_t* services);
void gui_framework_runtime_resize(int width, int height);
const gui_app_descriptor_t* gui_framework_descriptor(gui_app_id_t id);
unsigned int gui_framework_dispatch(gui_window_t* window,
                                    const gui_app_event_t* event);
unsigned int gui_framework_apply_result(gui_window_t* window,
                                        unsigned int result);
int gui_framework_request_close(gui_window_t* window);
void gui_framework_close_window(gui_window_t* window);
void gui_framework_close_all(void);
void gui_framework_sync_focus(void);
int gui_framework_dispatch_ticks(uint32_t now);
unsigned int gui_framework_poll_fds(struct pollfd* fds,
                                    gui_window_t** owners,
                                    unsigned int capacity);
int gui_framework_dispatch_ready_fds(void);
uint32_t gui_framework_deadline(uint32_t now);
void gui_framework_draw_client(gui_window_t* window, gfx_surface_t* desktop,
                               gui_rect_t client_screen,
                               const gui_rect_t* screen_clip,
                               int pointer_x, int pointer_y);

#endif
