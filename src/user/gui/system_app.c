#include "system_app.h"
#include "app_services.h"
#include "canvas.h"
#include "framework_internal.h"

#define TITLE_H 18
#define COL_FRAME 0x00000000u
#define COL_SUBTEXT 0x00404040u

static void copy_text(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    while (i + 1u < cap && src[i]) { dst[i] = src[i]; i++; }
    if (cap) dst[i] = 0;
}

static void append_text(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    while (i < cap && dst[i]) i++;
    while (i + 1u < cap && *src) dst[i++] = *src++;
    if (cap) dst[i < cap ? i : cap - 1u] = 0;
}

static void uint_text(unsigned int value, char* out) {
    char reverse[16]; int count = 0;
    if (!value) { out[0] = '0'; out[1] = 0; return; }
    while (value && count < 16) {
        reverse[count++] = (char)('0' + value % 10u); value /= 10u;
    }
    for (int i = 0; i < count; i++) out[i] = reverse[count - i - 1];
    out[count] = 0;
}

static void append_value(char* line, unsigned int capacity,
                         const char* label, unsigned int value) {
    char number[16];
    append_text(line, label, capacity);
    uint_text(value, number);
    append_text(line, number, capacity);
}

static void system_draw(gfx_surface_t* surface, gui_app_context_t* context,
                        int mouse_x, int mouse_y) {
    gui_window_t* window = context->window;
    gui_app_perf_snapshot_t perf;
    char line[96];
    (void)mouse_x; (void)mouse_y;
    gui_builtin_draw_system(surface,
        gui_rect_make(window->x, window->y + TITLE_H,
                      window->w, window->h - TITLE_H),
        gui_app_services_builtin_style(), gui_app_services_draw_text);
    if (gui_app_services_performance_visible()) {
        gui_app_services_performance_snapshot(&perf);
        copy_text(line, "Compose ", sizeof(line));
        append_value(line, sizeof(line), "", perf.composed_pixels);
        append_value(line, sizeof(line), "  Present ", perf.presented_pixels);
        gui_app_services_draw_text(surface, window->x + 8,
            window->y + window->h - 30, line, COL_SUBTEXT);
        copy_text(line, "Dirty ", sizeof(line));
        append_value(line, sizeof(line), "", perf.dirty_regions);
        append_value(line, sizeof(line), " Full ", perf.full_repaints);
        append_value(line, sizeof(line), " Idle ", perf.idle_wakeups);
        append_value(line, sizeof(line), " PTY ", perf.pty_wakeups);
        gui_app_services_draw_text(surface, window->x + 8,
            window->y + window->h - 17, line, COL_SUBTEXT);
    }
    gui_canvas_rect(surface, window->x, window->y,
                    window->w, window->h, COL_FRAME);
}

static const gui_app_descriptor_t DESCRIPTOR = {
    "System", 0, 280, 200, 200, 120, 0,
    0, 0, system_draw, 0, GUI_APP_SYSTEM, 0,
    "system", "System", 6, 1
};

const gui_app_descriptor_t* gui_system_app_descriptor(void) {
    return &DESCRIPTOR;
}
