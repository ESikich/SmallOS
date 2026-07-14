#include "desktop_shell.h"

#include "desktop_model.h"
#include "keyboard.h"
#include "preferences.h"
#include "theme.h"
#include "time.h"
#include "user_lib.h"
#include "widgets.h"
#include "window_manager.h"

#define TASKBAR_H 24
#define START_W 174
#define START_ROW_H 18
#define ICON_COUNT 2

typedef struct desktop_icon {
    int x, y;
    const char* label;
    gui_app_id_t app_id;
} desktop_icon_t;

typedef struct desktop_shell_state {
    int width, height;
    int start_open;
    int selection;
    int page;
    uint32_t clock_deadline;
    char notice[64];
    uint32_t notice_until;
    desktop_icon_t icons[ICON_COUNT];
    gui_desktop_shell_services_t services;
} desktop_shell_state_t;

static desktop_shell_state_t g_shell;

static void copy_text(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    if (!src) src = "";
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    if (cap) dst[i] = 0;
}

static void append_text(char* dst, const char* src, unsigned int cap) {
    unsigned int n = 0;
    while (n < cap && dst[n]) n++;
    while (n + 1 < cap && *src) dst[n++] = *src++;
    if (cap) dst[n] = 0;
}

static void number(unsigned int value, char* out) {
    char reverse[16]; int count = 0, index = 0;
    if (!value) { out[0] = '0'; out[1] = 0; return; }
    while (value && count < 16) {
        reverse[count++] = (char)('0' + value % 10u); value /= 10u;
    }
    while (count) out[index++] = reverse[--count];
    out[index] = 0;
}

static int contains(gui_rect_t r, int x, int y) {
    return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h;
}

static int entry_count(void) {
    return (int)gui_app_registry_launcher_count() + 1;
}

static const char* entry_label(int index) {
    const gui_app_descriptor_t* descriptor;
    if (index == (int)gui_app_registry_launcher_count()) return "Quit";
    descriptor = gui_app_registry_launcher_at((unsigned int)index);
    return descriptor ? descriptor->launcher_label : "";
}

static void activate_entry(int index) {
    const gui_app_descriptor_t* descriptor;
    if (index == (int)gui_app_registry_launcher_count()) {
        if (g_shell.services.request_quit) g_shell.services.request_quit();
        return;
    }
    descriptor = gui_app_registry_launcher_at((unsigned int)index);
    if (descriptor && g_shell.services.open_app)
        g_shell.services.open_app(descriptor->id, 0);
}

static int active_windows(void) {
    int count = 0;
    for (int i = 0; i < GUI_WINDOW_CAPACITY; i++)
        if (gui_wm_active(gui_wm_at(i))) count++;
    return count;
}

static gui_taskbar_layout_t layout(void) {
    gui_taskbar_layout_t value = gui_taskbar_layout(g_shell.width,
                                                     active_windows());
    g_shell.page = gui_taskbar_clamp_page(value, g_shell.page);
    return value;
}

static gui_rect_t task_button(int window_index) {
    gui_taskbar_layout_t value = layout();
    int x = value.first_x, ordinal = 0;
    int first = g_shell.page * value.per_page;
    int last = first + value.per_page;
    for (int i = 0; i < GUI_WINDOW_CAPACITY; i++) {
        if (!gui_wm_active(gui_wm_at(i))) continue;
        if (ordinal >= first && ordinal < last) {
            if (i == window_index)
                return gui_rect_make(x, g_shell.height - TASKBAR_H + 3,
                                     value.button_width - 3, TASKBAR_H - 6);
            x += value.button_width;
        }
        ordinal++;
    }
    return gui_rect_make(0, 0, 0, 0);
}

void gui_desktop_shell_init(int width, int height,
                            const gui_desktop_shell_services_t* services) {
    memset(&g_shell, 0, sizeof(g_shell));
    if (services) g_shell.services = *services;
    gui_desktop_shell_resize(width, height);
}

void gui_desktop_shell_resize(int width, int height) {
    g_shell.width = width; g_shell.height = height;
    g_shell.icons[0] = (desktop_icon_t){width - 80, 30, "Files", GUI_APP_FILES};
    g_shell.icons[1] = (desktop_icon_t){width - 80, 90, "Shell", GUI_APP_SHELL};
}

void gui_desktop_shell_set_notice(const char* text, uint32_t until) {
    copy_text(g_shell.notice, text, sizeof(g_shell.notice));
    g_shell.notice_until = until;
    if (g_shell.services.invalidate_taskbar)
        g_shell.services.invalidate_taskbar();
}

uint32_t gui_desktop_shell_deadline(void) { return g_shell.clock_deadline; }

int gui_desktop_shell_tick(uint32_t now) {
    if (!g_shell.clock_deadline) g_shell.clock_deadline = now + SMALLOS_TIMER_HZ * 60u;
    if ((int32_t)(now - g_shell.clock_deadline) < 0) return 0;
    g_shell.clock_deadline = now + SMALLOS_TIMER_HZ * 60u;
    if (g_shell.services.invalidate_taskbar) g_shell.services.invalidate_taskbar();
    return 1;
}

int gui_desktop_shell_start_open(void) { return g_shell.start_open; }

void gui_desktop_shell_toggle_start(void) {
    g_shell.start_open = !g_shell.start_open;
    g_shell.selection = 0;
    if (g_shell.services.invalidate_full) g_shell.services.invalidate_full();
}

gui_rect_t gui_desktop_shell_start_bounds(void) {
    int count = entry_count();
    return gui_rect_make(2, g_shell.height - TASKBAR_H - count * START_ROW_H - 4,
                         START_W, count * START_ROW_H + 4);
}

int gui_desktop_shell_key(unsigned int key, unsigned int ascii) {
    if (!g_shell.start_open) return 0;
    if (key == KEY_ESC) g_shell.start_open = 0;
    else if (key == KEY_UP)
        g_shell.selection = gui_start_move_selection(g_shell.selection,
                                                      entry_count(), -1);
    else if (key == KEY_DOWN || key == KEY_TAB)
        g_shell.selection = gui_start_move_selection(g_shell.selection,
                                                      entry_count(), 1);
    else if (key == KEY_ENTER) {
        int selected = g_shell.selection;
        g_shell.start_open = 0;
        if (g_shell.services.invalidate_full) g_shell.services.invalidate_full();
        activate_entry(selected);
        return 1;
    } else if (ascii >= 'a' && ascii <= 'z') {
        const char* labels[16]; int count = entry_count();
        for (int i = 0; i < count; i++) labels[i] = entry_label(i);
        int match = gui_start_first_letter(labels, count, (char)ascii);
        if (match >= 0) g_shell.selection = match;
    }
    if (g_shell.services.invalidate_full) g_shell.services.invalidate_full();
    return 1;
}

int gui_desktop_shell_click(int x, int y) {
    int bar_y = g_shell.height - TASKBAR_H;
    gui_rect_t start = gui_rect_make(4, bar_y + 3, 50, TASKBAR_H - 6);
    if (g_shell.start_open && contains(gui_desktop_shell_start_bounds(), x, y)) {
        gui_rect_t menu = gui_desktop_shell_start_bounds();
        int row = (y - menu.y - 2) / START_ROW_H;
        if (row >= 0 && row < entry_count()) {
            g_shell.selection = row; g_shell.start_open = 0;
            if (g_shell.services.invalidate_full) g_shell.services.invalidate_full();
            activate_entry(row);
        }
        return 1;
    }
    if (contains(start, x, y)) { gui_desktop_shell_toggle_start(); return 1; }
    if (y >= bar_y) {
        gui_taskbar_layout_t value = layout();
        if (value.paging && contains(gui_rect_make(58, bar_y + 3, 17, TASKBAR_H - 6), x, y)) {
            if (g_shell.page > 0) g_shell.page--;
            if (g_shell.services.invalidate_taskbar) g_shell.services.invalidate_taskbar();
            return 1;
        }
        if (value.paging && contains(gui_rect_make(g_shell.width - 207, bar_y + 3, 17, TASKBAR_H - 6), x, y)) {
            if (g_shell.page + 1 < value.page_count) g_shell.page++;
            if (g_shell.services.invalidate_taskbar) g_shell.services.invalidate_taskbar();
            return 1;
        }
        for (int i = 0; i < GUI_WINDOW_CAPACITY; i++) {
            gui_window_t* window = gui_wm_at(i);
            if (!gui_wm_active(window) || !contains(task_button(i), x, y)) continue;
            if (window == gui_wm_top() && !gui_wm_minimized(window)) {
                if (g_shell.services.minimize) g_shell.services.minimize(window);
            } else if (g_shell.services.restore_focus) {
                g_shell.services.restore_focus(window);
            }
            g_shell.start_open = 0;
            return 1;
        }
        g_shell.start_open = 0;
        return 1;
    }
    if (g_shell.start_open) {
        g_shell.start_open = 0;
        if (g_shell.services.invalidate_full) g_shell.services.invalidate_full();
        return 1;
    }
    return 0;
}

int gui_desktop_shell_icon_hit(int x, int y) {
    for (int i = 0; i < ICON_COUNT; i++)
        if (x >= g_shell.icons[i].x && x < g_shell.icons[i].x + 32 &&
            y >= g_shell.icons[i].y && y < g_shell.icons[i].y + 32) return i;
    return -1;
}

gui_rect_t gui_desktop_shell_icon_bounds(int index) {
    if (index < 0 || index >= ICON_COUNT) return gui_rect_make(0, 0, 0, 0);
    return gui_rect_make(g_shell.icons[index].x - 2,
                         g_shell.icons[index].y - 2, 36, 44);
}

static void draw_icon(gfx_surface_t* s, int index) {
    int x = g_shell.icons[index].x, y = g_shell.icons[index].y;
    if (index == 0) {
        gui_canvas_rect(s, x, y + 6, 28, 22, COL_FRAME);
        gui_canvas_hline(s, x + 2, y + 2, 12, COL_FRAME);
        gui_canvas_vline(s, x, y + 2, 4, COL_FRAME);
        gui_canvas_vline(s, x + 14, y + 2, 4, COL_FRAME);
        gui_canvas_fill_rect(s, x + 1, y + 7, 26, 20, 0x00FFFFE0u);
        gui_canvas_hline(s, x + 4, y + 12, 20, COL_FRAME);
        gui_canvas_hline(s, x + 4, y + 16, 20, COL_FRAME);
        gui_canvas_hline(s, x + 4, y + 20, 14, COL_FRAME);
    } else {
        gui_canvas_rect(s, x, y, 28, 22, COL_FRAME);
        gui_canvas_fill_rect(s, x + 2, y + 2, 24, 18, 0);
        gui_canvas_fill_rect(s, x + 4, y + 6, 6, 1, 0x00C8C8C8u);
        gui_canvas_fill_rect(s, x + 4, y + 10, 10, 1, 0x00C8C8C8u);
        gui_canvas_fill_rect(s, x + 12, y + 10, 2, 3, 0x00C8C8C8u);
    }
}

void gui_desktop_shell_draw_background(gfx_surface_t* s) {
    gui_rect_t clip = gui_canvas_has_clip() ? *gui_canvas_clip()
                                             : gui_rect_make(0, 0, s->width, s->height);
    for (int y = clip.y; y < clip.y + clip.h; y++) {
        unsigned int* row = s->pixels + y * s->pitch_pixels;
        for (int x = clip.x; x < clip.x + clip.w; x++)
            row[x] = ((x ^ y) & 1u) ? COL_DESKTOP_A : COL_DESKTOP_B;
    }
}

void gui_desktop_shell_draw_icons(gfx_surface_t* s, int hover) {
    for (int i = 0; i < ICON_COUNT; i++) {
        desktop_icon_t* icon = &g_shell.icons[i];
        if (i == hover)
            gui_canvas_fill_rect(s, icon->x - 2, icon->y - 2, 36, 44, COL_HILIGHT);
        draw_icon(s, i);
        gui_theme_draw_text(s, icon->x + 14 -
            (int)(gui_theme_text_width(icon->label) / 2u), icon->y + 34,
            icon->label, i == hover ? COL_HILIGHT_T : COL_TEXT);
    }
}

void gui_desktop_shell_draw_start(gfx_surface_t* s) {
    if (!g_shell.start_open) return;
    gui_rect_t menu = gui_desktop_shell_start_bounds();
    gui_canvas_fill_rect(s, menu.x, menu.y, menu.w, menu.h, COL_BAR);
    gui_canvas_rect(s, menu.x, menu.y, menu.w, menu.h, COL_FRAME);
    for (int i = 0; i < entry_count(); i++) {
        int y = menu.y + 2 + i * START_ROW_H;
        if (i == g_shell.selection)
            gui_canvas_fill_rect(s, menu.x + 2, y, menu.w - 4,
                                 START_ROW_H, COL_HILIGHT);
        gui_theme_draw_text(s, menu.x + 10, y + 6, entry_label(i),
            i == g_shell.selection ? COL_HILIGHT_T : COL_TEXT);
    }
}

void gui_desktop_shell_draw_taskbar(gfx_surface_t* s) {
    int y = g_shell.height - TASKBAR_H, x, ordinal = 0;
    gui_taskbar_layout_t value = layout();
    int first = g_shell.page * value.per_page, last = first + value.per_page;
    char status[96], num[16], clock_text[8] = "--:--";
    sys_fsinfo_t fs; struct timespec ts; struct tm tm;
    gui_canvas_fill_rect(s, 0, y, g_shell.width, TASKBAR_H, COL_BAR);
    gui_canvas_hline(s, 0, y, g_shell.width, COL_FRAME);
    gui_widget_button(s, gui_rect_make(4, y + 3, 50, TASKBAR_H - 6), "Start",
        (gui_widget_state_t){0, g_shell.start_open, g_shell.start_open, 0},
        &gui_retro_widget_theme, gui_theme_draw_text);
    if (value.paging) {
        gui_widget_button(s, gui_rect_make(58, y + 3, 17, TASKBAR_H - 6), "<",
            (gui_widget_state_t){0,0,0,g_shell.page == 0},
            &gui_retro_widget_theme, gui_theme_draw_text);
        gui_widget_button(s, gui_rect_make(g_shell.width - 207, y + 3, 17, TASKBAR_H - 6), ">",
            (gui_widget_state_t){0,0,0,g_shell.page + 1 >= value.page_count},
            &gui_retro_widget_theme, gui_theme_draw_text);
    }
    x = value.first_x;
    for (int i = 0; i < GUI_WINDOW_CAPACITY; i++) {
        gui_window_t* window = gui_wm_at(i);
        if (!gui_wm_active(window)) continue;
        if (ordinal >= first && ordinal < last) {
            gui_widget_button(s, gui_rect_make(x, y + 3, value.button_width - 3, TASKBAR_H - 6),
                gui_wm_title(window),
                (gui_widget_state_t){0,0,window == gui_wm_top() && !gui_wm_minimized(window),0},
                &gui_retro_widget_theme, gui_theme_draw_text);
            x += value.button_width;
        }
        ordinal++;
    }
    copy_text(status, "Free ", sizeof(status));
    if (sys_fsinfo(&fs) == 0) { number(fs.free_bytes / 1024u, num); append_text(status, num, sizeof(status)); append_text(status, "K", sizeof(status)); }
    else append_text(status, "?", sizeof(status));
    if (gui_preferences_performance_visible()) { append_text(status, " P", sizeof(status)); number(g_shell.services.shown_presents ? g_shell.services.shown_presents() : 0, num); append_text(status, num, sizeof(status)); }
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && gmtime_r(&ts.tv_sec, &tm)) {
        clock_text[0] = (char)('0' + tm.tm_hour / 10); clock_text[1] = (char)('0' + tm.tm_hour % 10);
        clock_text[3] = (char)('0' + tm.tm_min / 10); clock_text[4] = (char)('0' + tm.tm_min % 10);
    }
    gui_theme_draw_text(s, g_shell.width - 184, y + 9, status, COL_TEXT);
    gui_theme_draw_text(s, g_shell.width - 40, y + 9, clock_text, COL_TEXT);
    if (g_shell.notice[0] && (int32_t)(sys_get_ticks() - g_shell.notice_until) < 0) {
        int width = (int)gui_theme_text_width(g_shell.notice) + 12;
        gui_canvas_fill_rect(s, g_shell.width - width - 4, y - 20, width, 18, COL_WIN_BG);
        gui_canvas_rect(s, g_shell.width - width - 4, y - 20, width, 18, COL_FRAME);
        gui_theme_draw_text(s, g_shell.width - width + 2, y - 14, g_shell.notice, COL_TEXT);
    }
}
