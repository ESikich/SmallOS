#include "builtin_apps.h"

#include "user_lib.h"

static void builtin_copy(char* out, const char* text, unsigned int capacity) {
    unsigned int i = 0;
    while (text && text[i] && i + 1u < capacity) {
        out[i] = text[i];
        i++;
    }
    out[i] = '\0';
}

static void builtin_append(char* out, const char* text,
                           unsigned int capacity) {
    unsigned int used = 0;
    while (out[used]) used++;
    while (text && *text && used + 1u < capacity) out[used++] = *text++;
    out[used] = '\0';
}

static void builtin_uint(unsigned int value, char* out) {
    char reverse[16];
    int count = 0;
    if (value == 0u) {
        out[0] = '0';
        out[1] = '\0';
        return;
    }
    while (value && count < (int)sizeof(reverse)) {
        reverse[count++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    for (int i = 0; i < count; i++) out[i] = reverse[count - i - 1];
    out[count] = '\0';
}

static void builtin_metric(char* out, unsigned int capacity,
                           const char* label, unsigned int value,
                           const char* suffix) {
    char number[16];
    builtin_copy(out, label, capacity);
    builtin_uint(value, number);
    builtin_append(out, number, capacity);
    builtin_append(out, suffix, capacity);
}

void gui_builtin_draw_system(gfx_surface_t* surface, gui_rect_t bounds,
                             const gui_builtin_style_t* style,
                             gui_widget_text_fn draw_text) {
    sys_fsinfo_t fs;
    sys_display_info_t display;
    int has_fs = sys_fsinfo(&fs) == 0;
    int has_display = sys_display_info(&display) == 0;
    int y = bounds.y + 8;
    char text[64];
    char number[16];
    if (!surface || !style || !draw_text) return;
    gui_canvas_fill_rect(surface, bounds.x, bounds.y, bounds.w, bounds.h,
                         style->background);
    draw_text(surface, bounds.x + 8, y, "Display", style->subtext); y += 12;
    if (has_display) {
        builtin_metric(text, sizeof(text), "  ", display.width, " x ");
        builtin_uint(display.height, number); builtin_append(text, number, sizeof(text));
        builtin_append(text, " @ ", sizeof(text));
        builtin_uint(display.bpp, number); builtin_append(text, number, sizeof(text));
        builtin_append(text, " bpp", sizeof(text));
        draw_text(surface, bounds.x + 8, y, text, style->text); y += 12;
        builtin_metric(text, sizeof(text), "  pitch=", display.pitch, "");
        draw_text(surface, bounds.x + 8, y, text, style->text); y += 14;
    } else {
        draw_text(surface, bounds.x + 8, y, "  (unavailable)", style->text);
        y += 14;
    }
    draw_text(surface, bounds.x + 8, y, "Filesystem", style->subtext); y += 12;
    if (has_fs) {
        builtin_metric(text, sizeof(text), "  total: ",
                       fs.total_bytes / 1024u, " KB");
        draw_text(surface, bounds.x + 8, y, text, style->text); y += 12;
        builtin_metric(text, sizeof(text), "  used:  ",
                       fs.used_bytes / 1024u, " KB");
        draw_text(surface, bounds.x + 8, y, text, style->text); y += 12;
        builtin_metric(text, sizeof(text), "  free:  ",
                       fs.free_bytes / 1024u, " KB");
        draw_text(surface, bounds.x + 8, y, text, style->text); y += 12;
        builtin_metric(text, sizeof(text), "  blocks: ", fs.free_clusters, " / ");
        builtin_uint(fs.total_clusters, number); builtin_append(text, number, sizeof(text));
        builtin_append(text, " free", sizeof(text));
        draw_text(surface, bounds.x + 8, y, text, style->text); y += 14;
    } else {
        draw_text(surface, bounds.x + 8, y, "  (unavailable)", style->text);
        y += 14;
    }
    draw_text(surface, bounds.x + 8, y, "Process", style->subtext); y += 12;
    builtin_metric(text, sizeof(text), "  pid: ",
                   (unsigned int)sys_getpid(), "");
    draw_text(surface, bounds.x + 8, y, text, style->text); y += 12;
    builtin_metric(text, sizeof(text), "  ticks: ", sys_get_ticks(), "");
    draw_text(surface, bounds.x + 8, y, text, style->text);
}

void gui_builtin_draw_config(gfx_surface_t* surface, gui_rect_t bounds,
                             int perf_visible,
                             gui_widget_state_t control_state,
                             const gui_builtin_style_t* style,
                             const gui_widget_theme_t* theme,
                             gui_widget_text_fn draw_text) {
    if (!surface || !style || !theme || !draw_text) return;
    gui_canvas_fill_rect(surface, bounds.x, bounds.y, bounds.w, bounds.h,
                         style->background);
    draw_text(surface, bounds.x + 12, bounds.y + 12, "GUI", style->subtext);
    gui_widget_checkbox(surface,
        gui_rect_make(bounds.x + 8, bounds.y + 26, bounds.w - 16, 20),
        "Perf readout", perf_visible, control_state,
        theme, draw_text);
}

void gui_builtin_draw_about(gfx_surface_t* surface, gui_rect_t bounds,
                            const gui_builtin_style_t* style,
                            gui_widget_text_fn draw_text) {
    if (!surface || !style || !draw_text) return;
    gui_canvas_fill_rect(surface, bounds.x, bounds.y, bounds.w, bounds.h,
                         style->background);
    draw_text(surface, bounds.x + 12, bounds.y + 12,
              "SmallOS GUI", style->text);
    draw_text(surface, bounds.x + 12, bounds.y + 28,
              "Click an icon on the desktop", style->subtext);
    draw_text(surface, bounds.x + 12, bounds.y + 40,
              "to open a window.", style->subtext);
    draw_text(surface, bounds.x + 12, bounds.y + 60,
              "Drag the title bar to move.", style->subtext);
    draw_text(surface, bounds.x + 12, bounds.y + 72,
              "Click X to close.", style->subtext);
    draw_text(surface, bounds.x + 12, bounds.y + 92,
              "Press ESC or Q to exit gui.", style->subtext);
}
