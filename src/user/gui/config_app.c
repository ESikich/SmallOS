#include "config_app.h"
#include "app_services.h"
#include "keyboard.h"

static void config_draw(gfx_surface_t* surface, gui_app_context_t* context,
                        int mouse_x, int mouse_y) {
    (void)mouse_x; (void)mouse_y;
    gui_builtin_draw_config(surface,
        gui_rect_make(0, 0, (int)surface->width, (int)surface->height),
        gui_app_services_performance_visible(),
        (gui_widget_state_t){0, gui_app_captured_control(context) == 1,
                             gui_app_focused_control(context) == 1, 0},
        gui_app_services_builtin_style(), gui_app_services_widget_theme(),
        gui_app_services_draw_text);
}

static unsigned int config_event(gui_app_context_t* context,
                                 const gui_app_event_t* event) {
    int width = 0;
    gui_app_client_size(context, &width, 0);
    int inside = event->x >= 8 && event->x < width - 8 &&
                 event->y >= 26 && event->y < 46;
    if (event->type == GUI_APP_EVENT_POINTER_DOWN && inside) {
        gui_app_focus_control(context, 1);
        gui_app_capture_pointer(context, 1);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP &&
        gui_app_captured_control(context) == 1) {
        gui_app_release_pointer(context);
        if (!inside) return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        gui_app_services_toggle_performance();
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_KEY && event->key == KEY_TAB) {
        gui_app_focus_control(context, 1);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_KEY &&
        gui_app_focused_control(context) == 1 &&
        (event->key == KEY_ENTER || event->key == KEY_SPACE)) {
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
