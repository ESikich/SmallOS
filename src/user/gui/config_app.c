#include "config_app.h"
#include "app_services.h"
#include "canvas.h"
#include "framework_internal.h"
#include "keyboard.h"

#define TITLE_H 18
#define COL_FRAME 0x00000000u

static void config_draw(gfx_surface_t* surface, gui_app_context_t* context,
                        int mouse_x, int mouse_y) {
    gui_window_t* window = context->window;
    (void)mouse_x; (void)mouse_y;
    gui_builtin_draw_config(surface,
        gui_rect_make(window->x, window->y + TITLE_H,
                      window->w, window->h - TITLE_H),
        gui_app_services_performance_visible(),
        gui_app_services_builtin_style(), gui_app_services_widget_theme(),
        gui_app_services_draw_text);
    gui_canvas_rect(surface, window->x, window->y,
                    window->w, window->h, COL_FRAME);
}

static unsigned int config_event(gui_app_context_t* context,
                                 const gui_app_event_t* event) {
    gui_window_t* window = context->window;
    if ((event->type == GUI_APP_EVENT_POINTER_DOWN &&
         event->x >= 8 && event->x < window->w - 8 &&
         event->y >= 26 && event->y < 46) ||
        (event->type == GUI_APP_EVENT_KEY &&
         (event->key == KEY_ENTER || event->key == KEY_SPACE))) {
        gui_app_services_toggle_performance();
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    return GUI_APP_RESULT_NONE;
}

static const gui_app_descriptor_t DESCRIPTOR = {
    "Config", 0, 260, 116, 220, 100, 0,
    0, 0, config_draw, config_event, GUI_APP_CONFIG, 0,
    "config", "Config", 7, 1
};

const gui_app_descriptor_t* gui_config_app_descriptor(void) {
    return &DESCRIPTOR;
}
