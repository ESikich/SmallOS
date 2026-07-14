#include "window_manager.h"

#include <stdlib.h>
#include <string.h>

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

static gui_window_t g_windows[GUI_WINDOW_CAPACITY];
static gui_window_stack_t g_stack;
static int g_focused_window = -1;

void gui_wm_init(void) {
    memset(g_windows, 0, sizeof(g_windows));
    gui_window_stack_init(&g_stack);
    g_focused_window = -1;
}

gui_window_t* gui_wm_at(int index) {
    return index >= 0 && index < GUI_WINDOW_CAPACITY ? &g_windows[index] : 0;
}

int gui_wm_index(const gui_window_t* window) {
    if (!window || window < g_windows || window >= g_windows + GUI_WINDOW_CAPACITY)
        return -1;
    return (int)(window - g_windows);
}

void gui_wm_reset(gui_window_t* window) {
    if (window) memset(window, 0, sizeof(*window));
}

gui_window_t* gui_wm_allocate(void) {
    for (int i = 0; i < GUI_WINDOW_CAPACITY; i++) {
        if (!g_windows[i].active) {
            gui_wm_reset(&g_windows[i]);
            g_windows[i].active = 1;
            gui_window_stack_raise(&g_stack, i);
            return &g_windows[i];
        }
    }
    return 0;
}

void gui_wm_remove(gui_window_t* window) {
    int index = gui_wm_index(window);
    if (index < 0) return;
    gui_window_stack_remove(&g_stack, index);
    window->active = 0;
    if (g_focused_window == index) g_focused_window = -1;
}

void gui_wm_raise(gui_window_t* window) {
    int index = gui_wm_index(window);
    if (index >= 0 && window->active) gui_window_stack_raise(&g_stack, index);
}

gui_window_t* gui_wm_top(void) {
    for (int i = gui_window_stack_count(&g_stack) - 1; i >= 0; i--) {
        gui_window_t* window = gui_wm_at(gui_window_stack_at(&g_stack, i));
        if (window && window->active && !window->minimized) return window;
    }
    return 0;
}

gui_window_t* gui_wm_hit(int x, int y) {
    for (int i = gui_window_stack_count(&g_stack) - 1; i >= 0; i--) {
        gui_window_t* window = gui_wm_at(gui_window_stack_at(&g_stack, i));
        if (window && window->active && !window->minimized &&
            x >= window->x && x < window->x + window->w &&
            y >= window->y && y < window->y + window->h)
            return window;
    }
    return 0;
}

int gui_wm_stack_count(void) { return gui_window_stack_count(&g_stack); }

gui_window_t* gui_wm_stack_at(int position) {
    return gui_wm_at(gui_window_stack_at(&g_stack, position));
}

int gui_wm_focused_window(void) { return g_focused_window; }

void gui_wm_set_focused_window(int index) {
    g_focused_window = index >= -1 && index < GUI_WINDOW_CAPACITY ? index : -1;
}

int gui_wm_active(const gui_window_t* w) { return w && w->active; }
void gui_wm_set_active(gui_window_t* w, int v) { if (w) w->active = v; }
int gui_wm_minimized(const gui_window_t* w) { return w && w->minimized; }
void gui_wm_set_minimized(gui_window_t* w, int v) { if (w) w->minimized = v; }
int gui_wm_maximized(const gui_window_t* w) { return w && w->maximized; }
void gui_wm_set_maximized(gui_window_t* w, int v) { if (w) w->maximized = v; }
gui_app_id_t gui_wm_app_id(const gui_window_t* w) { return w ? w->type : 0; }
void gui_wm_set_app_id(gui_window_t* w, gui_app_id_t id) { if (w) w->type = id; }
int gui_wm_x(const gui_window_t* w) { return w ? w->x : 0; }
int gui_wm_y(const gui_window_t* w) { return w ? w->y : 0; }
int gui_wm_width(const gui_window_t* w) { return w ? w->w : 0; }
int gui_wm_height(const gui_window_t* w) { return w ? w->h : 0; }
void gui_wm_set_geometry(gui_window_t* w, int x, int y, int width, int height) {
    if (!w) return;
    w->x = x; w->y = y; w->w = width; w->h = height;
}
void gui_wm_restore_geometry(const gui_window_t* w, int* x, int* y,
                             int* width, int* height) {
    if (!w) return;
    if (x) *x = w->restore_x;
    if (y) *y = w->restore_y;
    if (width) *width = w->restore_w;
    if (height) *height = w->restore_h;
}
int gui_wm_restore_x(const gui_window_t* w) { return w ? w->restore_x : 0; }
int gui_wm_restore_y(const gui_window_t* w) { return w ? w->restore_y : 0; }
int gui_wm_restore_w(const gui_window_t* w) { return w ? w->restore_w : 0; }
int gui_wm_restore_h(const gui_window_t* w) { return w ? w->restore_h : 0; }
void gui_wm_set_restore_geometry(gui_window_t* w, int x, int y,
                                 int width, int height) {
    if (!w) return;
    w->restore_x = x; w->restore_y = y;
    w->restore_w = width; w->restore_h = height;
}
const char* gui_wm_title(const gui_window_t* w) { return w ? w->title : ""; }
void gui_wm_set_title(gui_window_t* w, const char* title) {
    unsigned int i = 0;
    if (!w) return;
    if (!title) title = "";
    while (i + 1 < sizeof(w->title) && title[i]) {
        w->title[i] = title[i]; i++;
    }
    w->title[i] = 0;
}
void* gui_wm_state(const gui_window_t* w) { return w ? w->state : 0; }
void gui_wm_set_state(gui_window_t* w, void* state) { if (w) w->state = state; }
int gui_wm_focused_control(const gui_window_t* w) { return w ? w->focused_widget : 0; }
void gui_wm_set_focused_control(gui_window_t* w, int c) { if (w) w->focused_widget = c; }
int gui_wm_captured_control(const gui_window_t* w) { return w ? w->pressed_widget : 0; }
void gui_wm_set_captured_control(gui_window_t* w, int c) { if (w) w->pressed_widget = c; }
uint32_t gui_wm_deadline(const gui_window_t* w) { return w ? w->next_tick : 0; }
void gui_wm_set_deadline(gui_window_t* w, uint32_t d) { if (w) w->next_tick = d; }
