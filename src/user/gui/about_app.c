#include "about_app.h"
#include "app_services.h"
#include "canvas.h"
#include "framework_internal.h"

#define TITLE_H 18
#define COL_FRAME 0x00000000u

static void about_draw(gfx_surface_t* surface, gui_app_context_t* context,
                       int mouse_x, int mouse_y) {
    gui_window_t* window = context->window;
    (void)mouse_x; (void)mouse_y;
    gui_builtin_draw_about(surface,
        gui_rect_make(window->x, window->y + TITLE_H,
                      window->w, window->h - TITLE_H),
        gui_app_services_builtin_style(), gui_app_services_draw_text);
    gui_canvas_rect(surface, window->x, window->y,
                    window->w, window->h, COL_FRAME);
}

static const gui_app_descriptor_t DESCRIPTOR = {
    "About", 0, 280, 140, 220, 100, 0,
    0, 0, about_draw, 0, GUI_APP_ABOUT, 0,
    "about", "About", 8, 1
};

const gui_app_descriptor_t* gui_about_app_descriptor(void) {
    return &DESCRIPTOR;
}
