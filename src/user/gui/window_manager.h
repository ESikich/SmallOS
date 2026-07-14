#ifndef SMALLOS_GUI_WINDOW_MANAGER_H
#define SMALLOS_GUI_WINDOW_MANAGER_H

#include "framework.h"
#include "window.h"

void gui_wm_init(void);
gui_window_t* gui_wm_at(int index);
int gui_wm_index(const gui_window_t* window);
void gui_wm_reset(gui_window_t* window);
gui_window_t* gui_wm_allocate(void);
void gui_wm_remove(gui_window_t* window);
void gui_wm_raise(gui_window_t* window);
gui_window_t* gui_wm_top(void);
gui_window_t* gui_wm_hit(int x, int y);
int gui_wm_stack_count(void);
gui_window_t* gui_wm_stack_at(int position);
int gui_wm_focused_window(void);
void gui_wm_set_focused_window(int index);

int gui_wm_active(const gui_window_t* window);
void gui_wm_set_active(gui_window_t* window, int value);
int gui_wm_minimized(const gui_window_t* window);
void gui_wm_set_minimized(gui_window_t* window, int value);
int gui_wm_maximized(const gui_window_t* window);
void gui_wm_set_maximized(gui_window_t* window, int value);
gui_app_id_t gui_wm_app_id(const gui_window_t* window);
void gui_wm_set_app_id(gui_window_t* window, gui_app_id_t id);

int gui_wm_x(const gui_window_t* window);
int gui_wm_y(const gui_window_t* window);
int gui_wm_width(const gui_window_t* window);
int gui_wm_height(const gui_window_t* window);
void gui_wm_set_geometry(gui_window_t* window, int x, int y,
                         int width, int height);
void gui_wm_restore_geometry(const gui_window_t* window, int* x, int* y,
                             int* width, int* height);
int gui_wm_restore_x(const gui_window_t* window);
int gui_wm_restore_y(const gui_window_t* window);
int gui_wm_restore_w(const gui_window_t* window);
int gui_wm_restore_h(const gui_window_t* window);
void gui_wm_set_restore_geometry(gui_window_t* window, int x, int y,
                                 int width, int height);

const char* gui_wm_title(const gui_window_t* window);
void gui_wm_set_title(gui_window_t* window, const char* title);
void* gui_wm_state(const gui_window_t* window);
void gui_wm_set_state(gui_window_t* window, void* state);
int gui_wm_focused_control(const gui_window_t* window);
void gui_wm_set_focused_control(gui_window_t* window, int control);
int gui_wm_captured_control(const gui_window_t* window);
void gui_wm_set_captured_control(gui_window_t* window, int control);
uint32_t gui_wm_deadline(const gui_window_t* window);
void gui_wm_set_deadline(gui_window_t* window, uint32_t deadline);

#endif
