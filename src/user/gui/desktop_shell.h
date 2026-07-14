#ifndef SMALLOS_GUI_DESKTOP_SHELL_H
#define SMALLOS_GUI_DESKTOP_SHELL_H

#include "canvas.h"
#include "framework.h"

typedef struct gui_desktop_shell_services {
    gui_window_t* (*open_app)(gui_app_id_t id, const char* argument);
    void (*request_quit)(void);
    void (*minimize)(gui_window_t* window);
    void (*restore_focus)(gui_window_t* window);
    void (*invalidate_full)(void);
    void (*invalidate_taskbar)(void);
    unsigned int (*shown_presents)(void);
} gui_desktop_shell_services_t;

void gui_desktop_shell_init(int width, int height,
                            const gui_desktop_shell_services_t* services);
void gui_desktop_shell_resize(int width, int height);
void gui_desktop_shell_set_notice(const char* text, uint32_t until);
uint32_t gui_desktop_shell_deadline(void);
int gui_desktop_shell_tick(uint32_t now);
int gui_desktop_shell_start_open(void);
void gui_desktop_shell_toggle_start(void);
int gui_desktop_shell_key(unsigned int key, unsigned int ascii);
int gui_desktop_shell_click(int x, int y);
int gui_desktop_shell_icon_hit(int x, int y);
gui_rect_t gui_desktop_shell_icon_bounds(int index);
gui_rect_t gui_desktop_shell_start_bounds(void);
void gui_desktop_shell_draw_background(gfx_surface_t* surface);
void gui_desktop_shell_draw_icons(gfx_surface_t* surface, int hover);
void gui_desktop_shell_draw_start(gfx_surface_t* surface);
void gui_desktop_shell_draw_taskbar(gfx_surface_t* surface);

#endif
