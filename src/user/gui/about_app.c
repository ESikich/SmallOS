#include "about_app.h"
#include "app_services.h"

static void about_draw(gfx_surface_t* surface, gui_app_context_t* context,
                       int mouse_x, int mouse_y) {
    (void)mouse_x; (void)mouse_y;
    gui_builtin_draw_about(surface,
        gui_rect_make(0, 0, (int)surface->width, (int)surface->height),
        gui_app_services_builtin_style(), gui_app_services_draw_text);
}

static const gui_app_descriptor_t DESCRIPTOR = {
    "About", 0, 280, 140, 220, 100, 0,
    0, 0, about_draw, 0, GUI_APP_ABOUT, 0,
    "about", "About", 8, 1
};

const gui_app_descriptor_t* gui_about_app_descriptor(void) {
    return &DESCRIPTOR;
}
