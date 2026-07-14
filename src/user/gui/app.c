/*
 * gui — interactive desktop for SmallOS.
 *
 * Real, working pieces (not a screenshot):
 *   - software mouse cursor driven by the kernel input queue
 *   - clickable desktop icons that open application windows
 *   - draggable windows with a working close button
 *   - z-order: clicking a window raises it to the front
 *   - "Files" window: live directory listing via opendir/readdir,
 *     click a row to descend, click ".." to ascend
 *   - "System" window: live sys_fsinfo, sys_display_info, sys_get_ticks, pid
 *   - "Config" window: persistent desktop settings
 *   - "About" window: built-in static info (no fake screens)
 *   - "Quit" icon exits and releases the display
 *
 * ESC also exits.
 */

#include "user_lib.h"
#include "gfx.h"
#include "smallos_input.h"
#include "keyboard.h"
#include "dirent.h"
#include "gui.h"
#include "canvas.h"
#include "app_event.h"
#include "about_app.h"
#include "app_services.h"
#include "builtin_apps.h"
#include "config_app.h"
#include "cursor.h"
#include "damage.h"
#include "desktop_model.h"
#include "editor_app.h"
#include "file_picker.h"
#include "files_app.h"
#include "framework.h"
#include "framework_internal.h"
#include "native_apps.h"
#include "occlusion.h"
#include "region.h"
#include "shell_window.h"
#include "system_app.h"
#include "widgets.h"
#include "window.h"
#include "../editor_model.h"
#include "time.h"

/* ---------------- colors ---------------- */

#define COL_DESKTOP_A 0x00C0C0B0u
#define COL_DESKTOP_B 0x00B0B0A0u
#define COL_WIN_BG    0x00FFFFFFu
#define COL_FRAME     0x00000000u
#define COL_TITLE_BG  0x00000000u
#define COL_TITLE_FG  0x00FFFFFFu
#define COL_TITLE_IDLE_BG 0x00808080u
#define COL_TEXT      0x00000000u
#define COL_SUBTEXT   0x00404040u
#define COL_HILIGHT   0x000060A0u
#define COL_HILIGHT_T 0x00FFFFFFu
#define COL_BTN_BG    0x00E0E0E0u
#define COL_BAR       0x00D4D0C8u
#define COL_SHADOW    0x00606060u

static const gui_widget_theme_t GUI_WIDGET_THEME = {
    COL_BTN_BG, 0x00D8D8D8u, 0x00A8A8A8u, COL_FRAME,
    COL_TEXT, 0x00808080u, COL_TITLE_BG
};

static const gui_builtin_style_t GUI_BUILTIN_STYLE = {
    COL_WIN_BG, COL_FRAME, COL_TEXT, COL_SUBTEXT
};

typedef int gui_fp_t;

#define make_rect gui_rect_make
#define rect_empty gui_rect_empty
#define rect_intersects gui_rect_intersects
#define rect_union gui_rect_union
#define rect_should_merge gui_rect_should_merge
#define rect_clip_screen gui_rect_clip
#define clip_set gui_canvas_set_clip
#define clip_clear gui_canvas_clear_clip
#define gui_put_pixel gui_canvas_put_pixel
#define fillr gui_canvas_fill_rect
#define hline gui_canvas_hline
#define vline gui_canvas_vline
#define rect gui_canvas_rect
#define draw_cursor gui_cursor_draw
#define cursor_screen_rect gui_cursor_rect
#define g_clip_enabled gui_canvas_has_clip()
#define g_clip_rect (*gui_canvas_clip())

/* ---------------- 5x7 bitmap font ---------------- */

typedef struct { char ch; unsigned char rows[7]; } glyph_t;
#define G(c, r0,r1,r2,r3,r4,r5,r6) { c, { r0,r1,r2,r3,r4,r5,r6 } }

static const glyph_t FONT[] = {
    G(' ',0,0,0,0,0,0,0), G('!',4,4,4,4,4,0,4), G('"',10,10,0,0,0,0,0),
    G('#',10,31,10,31,10,0,0), G('$',4,15,20,14,5,30,4),
    G('%',25,25,2,4,8,19,19), G('&',8,20,20,8,21,18,13),
    G('\'',4,4,0,0,0,0,0), G('(',2,4,8,8,8,4,2), G(')',8,4,2,2,2,4,8),
    G('*',0,4,21,14,21,4,0), G('+',0,4,4,31,4,4,0),
    G(',',0,0,0,0,0,4,8), G('-',0,0,0,31,0,0,0),
    G('.',0,0,0,0,0,12,12), G('/',1,2,2,4,8,8,16),
    G('0',14,17,19,21,25,17,14), G('1',4,12,4,4,4,4,14),
    G('2',14,17,1,2,4,8,31), G('3',30,1,1,14,1,1,30),
    G('4',2,6,10,18,31,2,2), G('5',31,16,16,30,1,1,30),
    G('6',14,16,16,30,17,17,14), G('7',31,1,2,4,8,8,8),
    G('8',14,17,17,14,17,17,14), G('9',14,17,17,15,1,1,14),
    G(':',0,12,12,0,12,12,0), G(';',0,12,12,0,12,4,8),
    G('<',2,4,8,16,8,4,2), G('=',0,0,31,0,31,0,0),
    G('>',16,8,4,2,4,8,16), G('?',14,17,1,2,4,0,4),
    G('@',14,17,1,13,21,21,14),
    G('A',14,17,17,31,17,17,17), G('B',30,17,17,30,17,17,30),
    G('C',14,17,16,16,16,17,14), G('D',30,17,17,17,17,17,30),
    G('E',31,16,16,30,16,16,31), G('F',31,16,16,30,16,16,16),
    G('G',14,17,16,23,17,17,14), G('H',17,17,17,31,17,17,17),
    G('I',14,4,4,4,4,4,14), G('J',7,2,2,2,2,18,12),
    G('K',17,18,20,24,20,18,17), G('L',16,16,16,16,16,16,31),
    G('M',17,27,21,21,17,17,17), G('N',17,25,21,19,17,17,17),
    G('O',14,17,17,17,17,17,14), G('P',30,17,17,30,16,16,16),
    G('Q',14,17,17,17,21,18,13), G('R',30,17,17,30,20,18,17),
    G('S',15,16,16,14,1,1,30), G('T',31,4,4,4,4,4,4),
    G('U',17,17,17,17,17,17,14), G('V',17,17,17,17,17,10,4),
    G('W',17,17,17,21,21,21,10), G('X',17,17,10,4,10,17,17),
    G('Y',17,17,17,10,4,4,4), G('Z',31,1,2,4,8,16,31),
    G('[',14,8,8,8,8,8,14), G('\\',16,8,8,4,2,2,1),
    G(']',14,2,2,2,2,2,14), G('^',4,10,17,0,0,0,0),
    G('_',0,0,0,0,0,0,31), G('`',8,4,0,0,0,0,0),
    G('a',0,0,14,1,15,17,15), G('b',16,16,22,25,17,17,30),
    G('c',0,0,14,16,16,17,14), G('d',1,1,13,19,17,17,15),
    G('e',0,0,14,17,31,16,14), G('f',6,9,8,28,8,8,8),
    G('g',0,0,15,17,15,1,14), G('h',16,16,22,25,17,17,17),
    G('i',4,0,12,4,4,4,14), G('j',2,0,6,2,2,2,18),
    G('k',16,16,18,20,24,20,18), G('l',12,4,4,4,4,4,14),
    G('m',0,0,26,21,21,17,17), G('n',0,0,22,25,17,17,17),
    G('o',0,0,14,17,17,17,14), G('p',0,0,30,17,30,16,16),
    G('q',0,0,15,17,15,1,1), G('r',0,0,22,25,16,16,16),
    G('s',0,0,14,16,14,1,30), G('t',8,8,28,8,8,9,6),
    G('u',0,0,17,17,17,19,13), G('v',0,0,17,17,17,10,4),
    G('w',0,0,17,17,21,21,10), G('x',0,0,17,10,4,10,17),
    G('y',0,0,17,17,15,1,14), G('z',0,0,31,2,4,8,31),
    G('{',6,8,8,16,8,8,6), G('|',4,4,4,4,4,4,4),
    G('}',12,2,2,1,2,2,12), G('~',8,21,2,0,0,0,0),
};
#define FONT_COUNT (sizeof(FONT) / sizeof(FONT[0]))

static const unsigned char* g_font_ascii[128];
static int g_font_ready = 0;

static void font_init_once(void) {
    if (g_font_ready) return;
    for (unsigned int i = 0; i < FONT_COUNT; i++) {
        unsigned char ch = (unsigned char)FONT[i].ch;
        if (ch < 128u) {
            g_font_ascii[ch] = FONT[i].rows;
        }
    }
    g_font_ready = 1;
}

static const unsigned char* glyph_for(char ch) {
    unsigned char uch = (unsigned char)ch;
    font_init_once();
    if (uch < 128u) {
        return g_font_ascii[uch];
    }
    return 0;
}

static void draw_char(gfx_surface_t* s, int x, int y, char ch, unsigned int color) {
    const unsigned char* g = glyph_for(ch);
    if (!g) { fillr(s, x + 1, y + 6, 2, 1, color); return; }
    for (unsigned int row = 0; row < 7; row++) {
        unsigned int bits = g[row];
        for (unsigned int col = 0; col < 5; col++) {
            if (bits & (1u << (4u - col))) {
                gui_put_pixel(s, x + (int)col, y + (int)row, color);
            }
        }
    }
}

static void draw_text(gfx_surface_t* s, int x, int y, const char* t, unsigned int c) {
    int cx = x;
    int x_limit;

    if (!s || !t) return;
    if (y + 7 <= 0 || y >= (int)s->height) return;
    x_limit = (int)s->width;
    if (g_clip_enabled) {
        if (y + 7 <= g_clip_rect.y || y >= g_clip_rect.y + g_clip_rect.h) return;
        x_limit = g_clip_rect.x + g_clip_rect.w;
        while (*t && cx + 5 < g_clip_rect.x) {
            cx += 6;
            t++;
        }
    } else {
        while (*t && cx + 5 < 0) {
            cx += 6;
            t++;
        }
    }
    while (*t && cx < x_limit) {
        draw_char(s, cx, y, *t, c);
        cx += 6;
        t++;
    }
}

static unsigned int text_width(const char* t) {
    unsigned int n = 0;
    while (*t) { n++; t++; }
    return n ? n * 6u - 1u : 0u;
}

static void draw_fixed_text(gfx_surface_t* s,
                            int x,
                            int y,
                            const char* t,
                            int max_chars,
                            unsigned int c) {
    int end = max_chars;
    int i = 0;
    int x_limit;
    if (!t || max_chars <= 0) return;
    if (!s || y + 7 <= 0 || y >= (int)s->height) return;
    x_limit = (int)s->width;
    if (g_clip_enabled) {
        if (y + 7 <= g_clip_rect.y || y >= g_clip_rect.y + g_clip_rect.h) return;
        x_limit = g_clip_rect.x + g_clip_rect.w;
        while (i < max_chars && x + i * 6 + 5 < g_clip_rect.x) i++;
    } else {
        while (i < max_chars && x + i * 6 + 5 < 0) i++;
    }
    while (end > 0 && t[end - 1] == ' ') end--;
    for (; i < end && t[i] && x + i * 6 < x_limit; i++) {
        draw_char(s, x + i * 6, y, t[i], c);
    }
}

/* ---------------- string helpers ---------------- */

static unsigned int u_strlen(const char* s) {
    unsigned int n = 0; while (s[n]) n++; return n;
}
static void u_strcpy_n(char* dst, const char* src, unsigned int cap) {
    unsigned int i = 0;
    while (i + 1 < cap && src[i]) { dst[i] = src[i]; i++; }
    if (cap) dst[i] = 0;
}
static void u_strcat_n(char* dst, const char* src, unsigned int cap) {
    unsigned int n = u_strlen(dst);
    while (n + 1 < cap && *src) { dst[n++] = *src++; }
    if (cap) dst[n] = 0;
}
static int u_streq(const char* a, const char* b) {
    while (*a && *a == *b) { a++; b++; }
    return *a == *b;
}
static void utoa10(unsigned int v, char* buf) {
    char tmp[16]; int n = 0;
    if (v == 0) { buf[0] = '0'; buf[1] = 0; return; }
    while (v > 0 && n < (int)sizeof(tmp)) { tmp[n++] = (char)('0' + v % 10u); v /= 10u; }
    int j = 0;
    while (n > 0) buf[j++] = tmp[--n];
    buf[j] = 0;
}

static unsigned int sample_self_ram(void) {
    sys_procinfo_t info;
    unsigned int pid = (unsigned int)sys_getpid();
    if (sys_procinfo(&info) < 0) return 0;
    for (unsigned int i = 0; i < info.out_count; i++)
        if (info.entries[i].pid == pid) return info.entries[i].ram_bytes;
    return 0;
}

static void diagnostic_append(char* line, unsigned int capacity,
                              const char* name, unsigned int value) {
    char number[16];
    u_strcat_n(line, " ", capacity);
    u_strcat_n(line, name, capacity);
    u_strcat_n(line, "=", capacity);
    utoa10(value, number);
    u_strcat_n(line, number, capacity);
}

/* ---------------- window model ---------------- */

#define MAX_WINDOWS GUI_WINDOW_CAPACITY
#define INPUT_BATCH 32
#define MOUSE_COALESCE_MAX INPUT_BATCH
#define MOUSE_MERGE_MAX_DELTA 48
#define GUI_PERF_WINDOW_TICKS SMALLOS_TIMER_HZ
#define GUI_FP_SHIFT 8
#define GUI_FP_ONE (1 << GUI_FP_SHIFT)
/*
 * The PIT runs at 300 Hz, so 60 FPS frame pacing is exactly 5 ticks.
 * Keep this derived so a later timer-rate change keeps the intent visible.
 */
#define GUI_FRAME_FPS 60u
#define GUI_FRAME_TICKS \
    ((SMALLOS_TIMER_HZ + GUI_FRAME_FPS - 1u) / GUI_FRAME_FPS)
#define GUI_CONFIG_PATH "/etc/gui.conf"
#define GUI_CONFIG_MAX 256u

#define WT_FILES GUI_APP_FILES
#define WT_SYSTEM GUI_APP_SYSTEM
#define WT_CONFIG GUI_APP_CONFIG
#define WT_ABOUT GUI_APP_ABOUT
#define WT_SHELL GUI_APP_SHELL
#define WT_EDITOR GUI_APP_EDITOR
typedef gui_app_id_t win_type_t;

typedef gui_window_t window_t;
typedef gui_app_descriptor_t window_app_ops_t;

static const window_app_ops_t* window_app_ops(win_type_t type);
static void action_editor(int sw, int sh, const char* path);
static void action_viewer(int sw, int sh, const char* path);

#define SHELL_STATE(w) ((gui_shell_window_t*)(w)->state)

static window_t g_wins[MAX_WINDOWS];
static int g_screen_width;
static int g_screen_height;

static int point_in(int x, int y, int rx, int ry, int rw, int rh);

#define TITLE_H 18
#define CLOSE_W 14
#define TITLE_BUTTON_W 14
#define ROW_H   12
#define RESIZE_GRIP 10
#define TASKBAR_H 24
#define START_MENU_W 174
#define START_MENU_ROW_H 18
#define WINDOW_MIN_W 160
#define WINDOW_MIN_H 80
#define CURSOR_W GUI_CURSOR_WIDTH
#define CURSOR_H GUI_CURSOR_HEIGHT
#define CURSOR_MOVE_MAX_W 64
#define CURSOR_MOVE_MAX_H 64

static gui_damage_t g_damage;
#define g_dirty (g_damage.rects)
#define g_dirty_count (g_damage.count)
#define g_dirty_full (g_damage.full)

typedef enum {
    DRAG_NONE = 0,
    DRAG_MOVE,
    DRAG_RESIZE,
} drag_mode_t;

static drag_mode_t g_drag = DRAG_NONE;
static int g_drag_idx = -1;
static gui_fp_t g_drag_dx_fp = 0, g_drag_dy_fp = 0;
static int g_drag_preview_x = 0, g_drag_preview_y = 0;
static int g_drag_overlay_visible = 0;
static int g_drag_overlay_x = 0, g_drag_overlay_y = 0;
static int g_resize_start_mx = 0, g_resize_start_my = 0;
static int g_resize_start_w = 0, g_resize_start_h = 0;
static int g_frame_pending = 0;
static uint32_t g_frame_next_tick = 0;
static int g_last_title_click_window = -1;
static uint32_t g_last_title_click_tick = 0;

static void icons_layout(int sw);

typedef struct {
    uint32_t window_start;
    uint32_t last_loop;
    unsigned int loops;
    unsigned int presents;
    unsigned int input_events;
    unsigned int max_loop_gap;
    unsigned int max_present_ticks;
    unsigned int composed_pixels;
    unsigned int presented_pixels;
    unsigned int dirty_regions;
    unsigned int full_repaints;
    unsigned int idle_wakeups;
    unsigned int pty_wakeups;
    unsigned int shown_loops;
    unsigned int shown_presents;
    unsigned int shown_input_events;
    unsigned int shown_max_loop_gap;
    unsigned int shown_max_present_ticks;
    unsigned int shown_composed_pixels;
    unsigned int shown_presented_pixels;
    unsigned int shown_dirty_regions;
    unsigned int shown_full_repaints;
    unsigned int shown_idle_wakeups;
    unsigned int shown_pty_wakeups;
    unsigned int total_loops;
    unsigned int total_presents;
    unsigned int total_input_events;
    unsigned int total_composed_pixels;
    unsigned int total_presented_pixels;
    unsigned int total_dirty_regions;
    unsigned int total_full_repaints;
    unsigned int total_idle_wakeups;
    unsigned int total_pty_wakeups;
} gui_perf_t;

static gui_perf_t g_perf;
static int g_perf_visible = 0;
static int g_diagnostics = 0;
static unsigned int g_startup_ram_bytes = 0;

static void diagnostics_print(void) {
    char line[384];
    u_strcpy_n(line, "gui: metrics", sizeof(line));
    diagnostic_append(line, sizeof(line), "startup_ram", g_startup_ram_bytes);
    diagnostic_append(line, sizeof(line), "saved_presentbuffer",
                      (unsigned int)g_screen_width *
                      (unsigned int)g_screen_height * 4u);
    diagnostic_append(line, sizeof(line), "loops", g_perf.total_loops);
    diagnostic_append(line, sizeof(line), "presents", g_perf.total_presents);
    diagnostic_append(line, sizeof(line), "input", g_perf.total_input_events);
    diagnostic_append(line, sizeof(line), "composed", g_perf.total_composed_pixels);
    diagnostic_append(line, sizeof(line), "presented", g_perf.total_presented_pixels);
    diagnostic_append(line, sizeof(line), "dirty", g_perf.total_dirty_regions);
    diagnostic_append(line, sizeof(line), "full", g_perf.total_full_repaints);
    diagnostic_append(line, sizeof(line), "idle", g_perf.total_idle_wakeups);
    diagnostic_append(line, sizeof(line), "pty", g_perf.total_pty_wakeups);
    u_strcat_n(line, "\n", sizeof(line));
    u_puts(line);
}

static int line_starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static const char* skip_config_spaces(const char* s) {
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

static void gui_config_parse_line(const char* line) {
    const char* p = skip_config_spaces(line);

    if (*p == 0 || *p == '#' || *p == ';') return;
    if (line_starts_with(p, "perf_visible")) {
        p += 12;
        p = skip_config_spaces(p);
        if (*p != '=') return;
        p = skip_config_spaces(p + 1);
        if (*p == '0') g_perf_visible = 0;
        else if (*p == '1') g_perf_visible = 1;
        return;
    }
    {
        const char* equals = p;
        char key[32];
        unsigned int length = 0;
        while (*equals && *equals != '=') equals++;
        if (*equals != '=') return;
        while (p < equals && *p != ' ' && *p != '\t' &&
               length + 1u < sizeof(key)) key[length++] = *p++;
        key[length] = 0;
        gui_native_network_pref_set(key, skip_config_spaces(equals + 1));
    }
}

static void gui_config_load(void) {
    char buf[GUI_CONFIG_MAX + 1u];
    char line[64];
    unsigned int line_len = 0;
    int fd = sys_open(GUI_CONFIG_PATH);
    int n;

    if (fd < 0) return;
    n = sys_fread(fd, buf, GUI_CONFIG_MAX);
    sys_close(fd);
    if (n <= 0) return;

    buf[n] = 0;
    for (int i = 0; i < n; i++) {
        char ch = buf[i];
        if (ch == '\r') continue;
        if (ch == '\n') {
            line[line_len] = 0;
            gui_config_parse_line(line);
            line_len = 0;
        } else if (line_len + 1u < sizeof(line)) {
            line[line_len++] = ch;
        }
    }
    if (line_len > 0) {
        line[line_len] = 0;
        gui_config_parse_line(line);
    }
}

static void gui_config_save(void) {
    char buf[GUI_CONFIG_MAX];
    int fd;

    u_strcpy_n(buf, "perf_visible=", sizeof(buf));
    u_strcat_n(buf, g_perf_visible ? "1\n" : "0\n", sizeof(buf));
    u_strcat_n(buf, "theme=retro\n", sizeof(buf));
    u_strcat_n(buf, "network_address=", sizeof(buf));
    u_strcat_n(buf, gui_native_network_pref_get("network_address"), sizeof(buf));
    u_strcat_n(buf, "\nnetwork_prefix=", sizeof(buf));
    u_strcat_n(buf, gui_native_network_pref_get("network_prefix"), sizeof(buf));
    u_strcat_n(buf, "\nnetwork_gateway=", sizeof(buf));
    u_strcat_n(buf, gui_native_network_pref_get("network_gateway"), sizeof(buf));
    u_strcat_n(buf, "\nnetwork_dns=", sizeof(buf));
    u_strcat_n(buf, gui_native_network_pref_get("network_dns"), sizeof(buf));
    u_strcat_n(buf, "\n", sizeof(buf));

    fd = sys_open_mode(GUI_CONFIG_PATH,
                       SYS_OPEN_MODE_WRITE |
                       SYS_OPEN_MODE_CREATE |
                       SYS_OPEN_MODE_TRUNC);
    if (fd < 0) return;
    (void)sys_writefd(fd, buf, u_strlen(buf));
    (void)sys_fsync(fd);
    (void)sys_close(fd);
}

void gui_app_services_draw_text(gfx_surface_t* surface, int x, int y,
                                const char* text, unsigned int color) {
    draw_text(surface, x, y, text, color);
}

const gui_builtin_style_t* gui_app_services_builtin_style(void) {
    return &GUI_BUILTIN_STYLE;
}

const gui_widget_theme_t* gui_app_services_widget_theme(void) {
    return &GUI_WIDGET_THEME;
}

int gui_app_services_performance_visible(void) {
    return g_perf_visible;
}

void gui_app_services_performance_snapshot(gui_app_perf_snapshot_t* snapshot) {
    if (!snapshot) return;
    snapshot->composed_pixels = g_perf.shown_composed_pixels;
    snapshot->presented_pixels = g_perf.shown_presented_pixels;
    snapshot->dirty_regions = g_perf.shown_dirty_regions;
    snapshot->full_repaints = g_perf.shown_full_repaints;
    snapshot->idle_wakeups = g_perf.shown_idle_wakeups;
    snapshot->pty_wakeups = g_perf.shown_pty_wakeups;
}

void gui_app_services_toggle_performance(void) {
    g_perf_visible = !g_perf_visible;
    gui_config_save();
}

void gui_preferences_save(void) {
    gui_config_save();
}

static gui_window_stack_t g_window_stack;
static int g_start_open = 0;
static int g_start_selection = 0;
static int g_taskbar_page = 0;
static int g_focused_window = -1;
static uint32_t g_clock_next_tick = 0;
static char g_desktop_notice[64];
static uint32_t g_desktop_notice_until = 0;
static gui_file_picker_t g_shared_picker;
static window_t* g_shared_picker_owner = 0;
static gui_file_filter_t g_shared_picker_filter = GUI_FILE_FILTER_ANY;
static int g_shared_picker_active = 0;

static const gui_file_picker_style_t SHARED_PICKER_STYLE = {
    COL_WIN_BG, COL_FRAME, COL_TEXT, COL_SUBTEXT,
    COL_HILIGHT, COL_HILIGHT_T
};

static void invalidate_rect(int sw, int sh, gui_rect_t r);
static void invalidate_topbar(int sw, int sh);

static int win_index(window_t* w) { return (int)(w - g_wins); }

static void z_remove_idx(int win_index_val) {
    gui_window_stack_remove(&g_window_stack, win_index_val);
}

static void z_push_top(int win_index_val) {
    gui_window_stack_raise(&g_window_stack, win_index_val);
}

static window_t* topmost(void) {
    for (int i = gui_window_stack_count(&g_window_stack) - 1; i >= 0; i--) {
        window_t* w = &g_wins[gui_window_stack_at(&g_window_stack, i)];
        if (w->active && !w->minimized) return w;
    }
    return 0;
}

static window_t* hit_window_z(int mx, int my) {
    for (int i = gui_window_stack_count(&g_window_stack) - 1; i >= 0; i--) {
        window_t* w = &g_wins[gui_window_stack_at(&g_window_stack, i)];
        if (!w->active || w->minimized) continue;
        if (mx >= w->x && mx < w->x + w->w &&
            my >= w->y && my < w->y + w->h) return w;
    }
    return 0;
}

static window_t* alloc_window(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_wins[i].active) {
            window_t* w = &g_wins[i];
            for (unsigned k = 0; k < sizeof(*w); k++) ((char*)w)[k] = 0;
            w->active = 1;
            z_push_top(i);
            return w;
        }
    }
    u_strcpy_n(g_desktop_notice, "Window limit reached", sizeof(g_desktop_notice));
    g_desktop_notice_until = sys_get_ticks() + SMALLOS_TIMER_HZ * 2u;
    invalidate_rect(g_screen_width, g_screen_height,
                    make_rect(0, g_screen_height - TASKBAR_H - 20,
                              g_screen_width, 20));
    return 0;
}

static void close_window(window_t* w) {
    const window_app_ops_t* ops;
    gui_app_context_t context;
    if (!w) return;
    if (g_shared_picker_active && g_shared_picker_owner == w) {
        g_shared_picker_active = 0;
        g_shared_picker_owner = 0;
    }
    ops = window_app_ops(w->type);
    context.window = w;
    context.state = w->state;
    if (ops && ops->close) ops->close(&context);
    if (w->state) free(w->state);
    w->state = 0;
    z_remove_idx(win_index(w));
    w->active = 0;
}

static gui_rect_t window_screen_rect(window_t* w) {
    if (!w || !w->active || w->minimized) return make_rect(0, 0, 0, 0);
    return make_rect(w->x, w->y, w->w + 3, w->h + 3);
}

static void dirty_clear(void) {
    gui_damage_clear(&g_damage);
}

static void invalidate_full(int sw, int sh) {
    gui_damage_full(&g_damage, sw, sh);
    g_perf.full_repaints++;
    g_perf.total_full_repaints++;
}

static void invalidate_rect(int sw, int sh, gui_rect_t r) {
    gui_damage_add(&g_damage, r, sw, sh);
}

static void invalidate_window(int sw, int sh, window_t* w) {
    invalidate_rect(sw, sh, window_screen_rect(w));
}

void gui_window_set_title(gui_window_t* window, const char* title) {
    if (!window) return;
    u_strcpy_n(window->title, title ? title : "",
               sizeof(window->title));
    invalidate_window(g_screen_width, g_screen_height, window);
    invalidate_topbar(g_screen_width, g_screen_height);
}

void gui_window_invalidate_local(gui_window_t* window,
                                 int x, int y, int width, int height) {
    if (!window || !window->active || window->minimized ||
        width <= 0 || height <= 0) return;
    invalidate_rect(g_screen_width, g_screen_height,
                    make_rect(window->x + x,
                              window->y + TITLE_H + y,
                              width, height));
}

void* gui_app_state(gui_app_context_t* context) {
    return context ? context->state : 0;
}

gui_window_t* gui_app_window(gui_app_context_t* context) {
    return context ? context->window : 0;
}

void gui_app_set_title(gui_app_context_t* context, const char* title) {
    if (context) gui_window_set_title(context->window, title);
}

void gui_app_invalidate(gui_app_context_t* context,
                        int x, int y, int width, int height) {
    if (context)
        gui_window_invalidate_local(context->window, x, y, width, height);
}

void gui_app_request_close(gui_app_context_t* context) {
    if (context) gui_window_request_close(context->window);
}

gui_window_t* gui_app_open(gui_app_context_t* context,
                           gui_app_id_t id, const char* argument) {
    (void)context;
    return gui_open_app(id, argument);
}

int gui_app_open_file_picker(gui_app_context_t* context,
                             gui_file_request_mode_t mode,
                             gui_file_filter_t filter,
                             const char* initial_path) {
    if (!context || !context->window || g_shared_picker_active) return 0;
    gui_file_picker_init(&g_shared_picker,
        mode == GUI_FILE_REQUEST_SAVE ? GUI_FILE_PICKER_SAVE
                                      : GUI_FILE_PICKER_OPEN,
        initial_path && initial_path[0] ? initial_path : "/");
    g_shared_picker_owner = context->window;
    g_shared_picker_filter = filter;
    g_shared_picker_active = 1;
    invalidate_full(g_screen_width, g_screen_height);
    return 1;
}

static void invalidate_topbar(int sw, int sh) {
    invalidate_rect(sw, sh, make_rect(0, sh - TASKBAR_H, sw, TASKBAR_H));
}

static void perf_init(void) {
    uint32_t now = sys_get_ticks();

    g_perf.window_start = now;
    g_perf.last_loop = now;
    g_perf.loops = 0;
    g_perf.presents = 0;
    g_perf.input_events = 0;
    g_perf.max_loop_gap = 0;
    g_perf.max_present_ticks = 0;
    g_perf.shown_loops = 0;
    g_perf.shown_presents = 0;
    g_perf.shown_input_events = 0;
    g_perf.shown_max_loop_gap = 0;
    g_perf.shown_max_present_ticks = 0;
    g_perf.composed_pixels = g_perf.presented_pixels = 0;
    g_perf.dirty_regions = g_perf.full_repaints = 0;
    g_perf.idle_wakeups = g_perf.pty_wakeups = 0;
    g_perf.shown_composed_pixels = g_perf.shown_presented_pixels = 0;
    g_perf.shown_dirty_regions = g_perf.shown_full_repaints = 0;
    g_perf.shown_idle_wakeups = g_perf.shown_pty_wakeups = 0;
    g_perf.total_loops = g_perf.total_presents = 0;
    g_perf.total_input_events = 0;
    g_perf.total_composed_pixels = g_perf.total_presented_pixels = 0;
    g_perf.total_dirty_regions = g_perf.total_full_repaints = 0;
    g_perf.total_idle_wakeups = g_perf.total_pty_wakeups = 0;
}

static int perf_tick(int sw, int sh) {
    uint32_t now = sys_get_ticks();
    unsigned int gap = now - g_perf.last_loop;

    g_perf.last_loop = now;
    g_perf.loops++;
    g_perf.total_loops++;
    if (gap > g_perf.max_loop_gap) {
        g_perf.max_loop_gap = gap;
    }

    if ((uint32_t)(now - g_perf.window_start) >= GUI_PERF_WINDOW_TICKS) {
        g_perf.shown_loops = g_perf.loops;
        g_perf.shown_presents = g_perf.presents;
        g_perf.shown_input_events = g_perf.input_events;
        g_perf.shown_max_loop_gap = g_perf.max_loop_gap;
        g_perf.shown_max_present_ticks = g_perf.max_present_ticks;
        g_perf.shown_composed_pixels = g_perf.composed_pixels;
        g_perf.shown_presented_pixels = g_perf.presented_pixels;
        g_perf.shown_dirty_regions = g_perf.dirty_regions;
        g_perf.shown_full_repaints = g_perf.full_repaints;
        g_perf.shown_idle_wakeups = g_perf.idle_wakeups;
        g_perf.shown_pty_wakeups = g_perf.pty_wakeups;
        g_perf.window_start = now;
        g_perf.loops = 0;
        g_perf.presents = 0;
        g_perf.input_events = 0;
        g_perf.max_loop_gap = 0;
        g_perf.max_present_ticks = 0;
        g_perf.composed_pixels = g_perf.presented_pixels = 0;
        g_perf.dirty_regions = g_perf.full_repaints = 0;
        g_perf.idle_wakeups = g_perf.pty_wakeups = 0;
        invalidate_topbar(sw, sh);
        return 1;
    }

    return 0;
}

static void perf_note_input(unsigned int events) {
    g_perf.input_events += events;
    g_perf.total_input_events += events;
}

static void perf_note_present(uint32_t start_tick) {
    uint32_t now = sys_get_ticks();
    unsigned int ticks = now - start_tick;

    g_perf.presents++;
    g_perf.total_presents++;
    if (ticks > g_perf.max_present_ticks) {
        g_perf.max_present_ticks = ticks;
    }
}

static void perf_resume_from_idle(void) {
    uint32_t now = sys_get_ticks();
    g_perf.last_loop = now;
    g_perf.idle_wakeups++;
    g_perf.total_idle_wakeups++;
}

static gui_rect_t drag_preview_rect(void) {
    if (g_drag != DRAG_MOVE || g_drag_idx < 0 || g_drag_idx >= MAX_WINDOWS ||
        !g_wins[g_drag_idx].active) {
        return make_rect(0, 0, 0, 0);
    }
    return make_rect(g_drag_preview_x, g_drag_preview_y,
                     g_wins[g_drag_idx].w, g_wins[g_drag_idx].h);
}

static gui_rect_t drag_overlay_rect(void) {
    if (!g_drag_overlay_visible ||
        g_drag != DRAG_MOVE ||
        g_drag_idx < 0 ||
        g_drag_idx >= MAX_WINDOWS ||
        !g_wins[g_drag_idx].active) {
        return make_rect(0, 0, 0, 0);
    }
    return make_rect(g_drag_overlay_x, g_drag_overlay_y,
                     g_wins[g_drag_idx].w, g_wins[g_drag_idx].h);
}

static int drag_target_pending(void) {
    return g_drag == DRAG_MOVE &&
           g_drag_idx >= 0 &&
           g_drag_idx < MAX_WINDOWS &&
           g_wins[g_drag_idx].active &&
           (!g_drag_overlay_visible ||
            g_drag_overlay_x != g_drag_preview_x ||
            g_drag_overlay_y != g_drag_preview_y);
}

static void gui_request_frame(uint32_t now) {
    g_frame_pending = 1;
    if (g_frame_next_tick == 0u || (int)(now - g_frame_next_tick) >= 0) {
        g_frame_next_tick = now;
    }
}

static void gui_request_frame_now(uint32_t now) {
    g_frame_pending = 1;
    g_frame_next_tick = now;
}

static int gui_frame_pending(void) {
    return g_frame_pending;
}

static int gui_frame_due(uint32_t now) {
    return g_frame_pending && (int)(now - g_frame_next_tick) >= 0;
}

static uint32_t gui_frame_deadline(void) {
    return g_frame_next_tick;
}

static void gui_finish_frame(uint32_t now) {
    g_frame_pending = 0;
    g_frame_next_tick = now + GUI_FRAME_TICKS;
}

static void gui_cancel_frame(void) {
    g_frame_pending = 0;
    g_frame_next_tick = 0;
}

static int tick_due(uint32_t now, uint32_t deadline) {
    return (int)(now - deadline) >= 0;
}

static uint32_t min_deadline(uint32_t a, uint32_t b) {
    return tick_due(a, b) ? b : a;
}

/* ---------------- file launcher ---------------- */

typedef enum {
    LAUNCH_NONE = 0,
    LAUNCH_ELF,
    LAUNCH_EDIT,
    LAUNCH_BMP,
} launch_kind_t;

static launch_kind_t g_launch_kind = LAUNCH_NONE;
static char g_launch_path[256];

static int s_ends_with(const char* s, const char* suffix) {
    unsigned int n = u_strlen(s);
    unsigned int m = u_strlen(suffix);
    if (m > n) return 0;
    return u_streq(s + n - m, suffix);
}

static int s_starts_with(const char* s, const char* prefix) {
    while (*prefix) {
        if (*s++ != *prefix++) return 0;
    }
    return 1;
}

static void basename_of(const char* path, char* out, unsigned int cap) {
    const char* base = path;
    const char* p = path;
    while (*p) {
        if (*p == '/') base = p + 1;
        p++;
    }
    u_strcpy_n(out, base, cap);
}

static int is_text_like_file(const char* path) {
    return s_ends_with(path, ".txt") ||
           s_ends_with(path, ".TXT") ||
           s_ends_with(path, ".c") ||
           s_ends_with(path, ".h") ||
           s_ends_with(path, ".md") ||
           s_ends_with(path, ".ini") ||
           s_ends_with(path, ".log") ||
           s_ends_with(path, ".html");
}

static int is_program_path(const char* path) {
    const char* base = path;
    const char* p = path;

    while (*p) {
        if (*p == '/') base = p + 1;
        p++;
    }
    if (!base[0]) return 0;
    for (p = base; *p; p++) {
        if (*p == '.') return 0;
    }

    return s_starts_with(path, "/bin/") ||
           s_starts_with(path, "/usr/bin/") ||
           s_starts_with(path, "/usr/sbin/") ||
           s_starts_with(path, "/usr/libexec/tests/");
}

static launch_kind_t launch_kind_for_path(const char* path) {
    if (s_ends_with(path, ".elf")) return LAUNCH_ELF;
    if (is_program_path(path)) return LAUNCH_ELF;
    if (s_ends_with(path, ".bmp") || s_ends_with(path, ".BMP")) return LAUNCH_BMP;
    if (is_text_like_file(path)) return LAUNCH_EDIT;
    return LAUNCH_NONE;
}

int gui_app_services_open_path(gui_app_context_t* context, const char* path) {
    launch_kind_t kind = launch_kind_for_path(path);
    (void)context;
    if (kind == LAUNCH_EDIT) {
        return gui_app_open(context, GUI_APP_EDITOR, path) ? 1 : 0;
    }
    if (kind == LAUNCH_BMP) {
        return gui_app_open(context, GUI_APP_VIEWER, path) ? 2 : 0;
    }
    if (kind == LAUNCH_ELF) {
        g_launch_kind = kind;
        u_strcpy_n(g_launch_path, path, sizeof(g_launch_path));
        return 3;
    }
    return 0;
}

static int run_queued_launch(gfx_context_t* gfx, int* sw, int* sh) {
    launch_kind_t kind = g_launch_kind;
    char path[256];
    char name[NAME_MAX + 1];
    char* argv[3];
    int pid;
    int status = 0;

    if (kind == LAUNCH_NONE) return 0;

    u_strcpy_n(path, g_launch_path, sizeof(path));
    g_launch_kind = LAUNCH_NONE;
    g_launch_path[0] = 0;

    gfx_close(gfx);

    if (kind == LAUNCH_ELF) {
        basename_of(path, name, sizeof(name));
        argv[0] = name;
        argv[1] = 0;
        pid = sys_exec_foreground(path, 1, argv);
    } else if (kind == LAUNCH_BMP) {
        argv[0] = "bmpview";
        argv[1] = path;
        argv[2] = 0;
        pid = sys_exec_foreground("/bin/bmpview", 2, argv);
    } else {
        argv[0] = "edit";
        argv[1] = path;
        argv[2] = 0;
        pid = sys_exec_foreground("/bin/edit", 2, argv);
    }

    if (pid >= 0) {
        if (sys_waitpid_foreground(pid, &status) < 0) {
            (void)sys_waitpid(pid, &status, 0);
        }
    } else {
        u_puts("gui: launch failed: ");
        u_puts(path);
        u_putc('\n');
        sys_sleep(30);
    }

    if (gfx_open(gfx) < 0) {
        u_puts("gui: could not reacquire display\n");
        return -1;
    }

    *sw = (int)gfx->backbuffer.width;
    *sh = (int)gfx->backbuffer.height;
    g_screen_width = *sw;
    g_screen_height = *sh;
    icons_layout(*sw);
    return 1;
}

/* ---------------- drawing windows ---------------- */

static void draw_title_button(gfx_surface_t* s, int x, int y, int kind) {
    fillr(s, x, y, TITLE_BUTTON_W - 2, TITLE_H - 4, COL_WIN_BG);
    rect(s, x, y, TITLE_BUTTON_W - 2, TITLE_H - 4, COL_FRAME);
    if (kind == 0) {
        hline(s, x + 3, y + TITLE_H - 8, 6, COL_FRAME);
    } else if (kind == 1) {
        rect(s, x + 3, y + 3, 6, 6, COL_FRAME);
    } else {
        int ix = x + 3, iy = y + 3;
        for (int i = 0; i < 6; i++) {
            gui_put_pixel(s, ix + i, iy + i, COL_FRAME);
            gui_put_pixel(s, ix + 5 - i, iy + i, COL_FRAME);
        }
    }
}

static void draw_title_bar(gfx_surface_t* s, window_t* w, int focused, const char* title) {
    unsigned int bg = focused ? COL_TITLE_BG : COL_TITLE_IDLE_BG;
    fillr(s, w->x, w->y, w->w, TITLE_H, bg);
    draw_text(s, w->x + 4, w->y + 6, title, COL_TITLE_FG);
    int cx = w->x + w->w - CLOSE_W - 2;
    int cy = w->y + 2;
    draw_title_button(s, cx - TITLE_BUTTON_W * 2, cy, 0);
    draw_title_button(s, cx - TITLE_BUTTON_W, cy, 1);
    draw_title_button(s, cx, cy, 2);
}

static void draw_shell_body(gfx_surface_t* s, window_t* w) {
    gui_shell_window_t* shell = SHELL_STATE(w);
    int bx = w->x;
    int by = w->y + TITLE_H;
    int bw = w->w;
    int bh = w->h - TITLE_H;
    fillr(s, bx, by, bw, bh, 0x00000000u);

    int line_h = 8;
    int pad = 4;
    int pty_mode = shell->backend == GUI_SHELL_BACKEND_PTY_CHILD;
    int input_h = pty_mode ? 0 : line_h + 2;
    int rows_area = bh - input_h - pad * 2;
    int visible = rows_area / line_h;
    if (visible < 1) visible = 1;
    int cols = (bw - 8) / 6;
    if (cols < 1) cols = 1;
    if (cols > GUI_SHELL_COLS) cols = GUI_SHELL_COLS;
    gui_shell_set_terminal_size(shell, (unsigned int)visible, (unsigned int)cols);

    int start = shell->line_count - visible - shell->scroll;
    if (start < 0) start = 0;
    int ty = by + pad;
    for (int i = 0; i < visible; i++) {
        if ((start + i) >= shell->line_count) break;
        draw_fixed_text(s, bx + 4, ty, shell->lines[start + i], cols, 0x00C8C8C8u);
        ty += line_h;
    }

    if (pty_mode) {
        int cursor_row = shell->cursor_row - start;
        int cursor_col = shell->cursor_col;
        if (cursor_row >= 0 && cursor_row < visible) {
            if (cursor_col < 0) cursor_col = 0;
            if (cursor_col > cols) cursor_col = cols;
            fillr(s,
                  bx + 4 + cursor_col * 6,
                  by + pad + cursor_row * line_h,
                  5,
                  7,
                  0x00FFFFFFu);
        }
        rect(s, w->x, w->y, w->w, w->h, COL_FRAME);
        return;
    }

    /* input prompt at bottom */
    int iy = by + bh - input_h - 2;
    hline(s, bx, iy - 2, bw, 0x00404040u);
    char prompt[GUI_SHELL_COLS + 1];
    gui_shell_format_prompt(shell, prompt, sizeof(prompt));
    draw_text(s, bx + 4, iy, prompt, 0x00FFFFFFu);
    /* cursor: a solid block right after input */
    int cursor_chars = (shell->backend == GUI_SHELL_BACKEND_PTY_CHILD)
        ? shell->pending_cursor
        : (int)u_strlen(prompt);
    if (cursor_chars < 0) cursor_chars = 0;
    if (cursor_chars > (int)u_strlen(prompt)) cursor_chars = (int)u_strlen(prompt);
    char cursor_prefix[GUI_SHELL_COLS + 1];
    cursor_prefix[0] = 0;
    for (int i = 0; i < cursor_chars && i < GUI_SHELL_COLS; i++) {
        cursor_prefix[i] = prompt[i];
        cursor_prefix[i + 1] = 0;
    }
    int cur_x = bx + 4 + (int)text_width(cursor_prefix) + 1;
    fillr(s, cur_x, iy, 5, 7, 0x00FFFFFFu);

    rect(s, w->x, w->y, w->w, w->h, COL_FRAME);
}

static void shell_app_open(gui_app_context_t* context, const char* argument) {
    window_t* w = context->window;
    (void)argument;
    gui_shell_open(SHELL_STATE(w));
}

static void shell_app_close(gui_app_context_t* context) {
    window_t* w = context->window;
    if (w->state) gui_shell_close(SHELL_STATE(w));
}

static void shell_app_draw(gfx_surface_t* s, gui_app_context_t* context,
                           int mx, int my) {
    window_t* w = context->window;
    (void)mx;
    (void)my;
    draw_shell_body(s, w);
}

static unsigned int shell_app_event(gui_app_context_t* context,
                                    const gui_app_event_t* event) {
    window_t* w = context->window;
    gui_shell_window_t* shell = SHELL_STATE(w);
    if (event->type == GUI_APP_EVENT_KEY) {
        if (event->key == KEY_ESC || (event->ascii & 0xFFu) == 27u)
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_CLOSE;
        if (gui_shell_handle_key(shell, event->ascii & 0xFFu, event->key,
                                 event->modifiers) == GUI_SHELL_KEY_CLOSE)
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_CLOSE;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_WHEEL) {
        int max_scroll = shell->line_count - 1;
        if (max_scroll < 0) max_scroll = 0;
        shell->scroll += event->wheel * 3;
        if (shell->scroll < 0) shell->scroll = 0;
        if (shell->scroll > max_scroll) shell->scroll = max_scroll;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_TICK) {
        return gui_shell_poll(shell) ? GUI_APP_RESULT_REDRAW
                                     : GUI_APP_RESULT_NONE;
    }
    return GUI_APP_RESULT_NONE;
}

static const window_app_ops_t SHELL_DESCRIPTOR = {
    "Shell", sizeof(gui_shell_window_t), 500, 320, 240, 120,
     0, shell_app_open, shell_app_close,
     shell_app_draw, shell_app_event, WT_SHELL, 1, "shell", "Shell", 2, 1
};

static void init_app_registry(void) {
    gui_app_registry_reset();
    (void)gui_app_registry_add(&SHELL_DESCRIPTOR);
    (void)gui_app_registry_add(gui_system_app_descriptor());
    (void)gui_app_registry_add(gui_config_app_descriptor());
    (void)gui_app_registry_add(gui_about_app_descriptor());
    (void)gui_app_registry_add(gui_files_app_descriptor());
    (void)gui_app_registry_add(gui_editor_app_descriptor());
    (void)gui_app_registry_add(gui_native_app_descriptor(GUI_APP_VIEWER));
    (void)gui_app_registry_add(gui_native_app_descriptor(GUI_APP_TASKS));
    (void)gui_app_registry_add(gui_native_app_descriptor(GUI_APP_NETWORK));
}

static const window_app_ops_t* window_app_ops(win_type_t type) {
    return gui_app_registry_find(type);
}

static unsigned int dispatch_app_event(window_t* w,
                                       const gui_app_event_t* event) {
    const window_app_ops_t* ops = w ? window_app_ops(w->type) : 0;
    gui_app_context_t context;
    if (!w || !w->active || !ops || !ops->event) return GUI_APP_RESULT_NONE;
    context.window = w;
    context.state = w->state;
    return ops->event(&context, event);
}

static int request_window_close(window_t* w, int sw, int sh) {
    gui_app_event_t event;
    unsigned int result;
    if (!w || !w->active) return 1;
    memset(&event, 0, sizeof(event));
    event.type = GUI_APP_EVENT_CLOSE_REQUEST;
    event.width = w->w;
    event.height = w->h - TITLE_H;
    event.ticks = sys_get_ticks();
    result = dispatch_app_event(w, &event);
    if (result & GUI_APP_RESULT_REDRAW) invalidate_window(sw, sh, w);
    if (result & GUI_APP_RESULT_KEEP_OPEN) return 0;
    invalidate_window(sw, sh, w);
    close_window(w);
    invalidate_topbar(sw, sh);
    return 1;
}

static unsigned int apply_app_result(window_t* w, unsigned int result,
                                     int sw, int sh) {
    if ((result & GUI_APP_RESULT_REDRAW) && w && w->active)
        invalidate_window(sw, sh, w);
    if ((result & GUI_APP_RESULT_CLOSE) && w && w->active)
        (void)request_window_close(w, sw, sh);
    return result;
}

static gui_rect_t shared_picker_bounds(void) {
    int width = g_screen_width - 80;
    int height = g_screen_height - TASKBAR_H - 80;
    if (width > 560) width = 560;
    if (height > 400) height = 400;
    if (width < 280) width = 280;
    if (height < 180) height = 180;
    return make_rect((g_screen_width - width) / 2,
                     (g_screen_height - TASKBAR_H - height) / 2,
                     width, height);
}

static int shared_picker_path_allowed(const char* path) {
    if (g_shared_picker_filter == GUI_FILE_FILTER_ANY) return 1;
    if (g_shared_picker_filter == GUI_FILE_FILTER_BMP)
        return s_ends_with(path, ".bmp") || s_ends_with(path, ".BMP");
    return is_text_like_file(path);
}

static int handle_shared_picker_event(const gui_app_event_t* event,
                                      int sw, int sh) {
    gui_file_picker_result_t result;
    gui_app_event_t completion;
    window_t* owner = g_shared_picker_owner;
    if (!g_shared_picker_active) return 0;
    result = gui_file_picker_event(&g_shared_picker, event,
                                   shared_picker_bounds());
    if (result == GUI_FILE_PICKER_RESULT_ACCEPT &&
        !shared_picker_path_allowed(gui_file_picker_path(&g_shared_picker))) {
        u_strcpy_n(g_desktop_notice,
                   g_shared_picker_filter == GUI_FILE_FILTER_BMP
                   ? "Choose a BMP file" : "Choose a text file",
                   sizeof(g_desktop_notice));
        g_desktop_notice_until = sys_get_ticks() + SMALLOS_TIMER_HZ * 2u;
        invalidate_full(sw, sh);
        return 1;
    }
    if (result == GUI_FILE_PICKER_RESULT_ACCEPT ||
        result == GUI_FILE_PICKER_RESULT_CANCEL) {
        memset(&completion, 0, sizeof(completion));
        completion.type = result == GUI_FILE_PICKER_RESULT_ACCEPT
                        ? GUI_APP_EVENT_FILE_SELECTED
                        : GUI_APP_EVENT_FILE_CANCELLED;
        completion.path = result == GUI_FILE_PICKER_RESULT_ACCEPT
                        ? gui_file_picker_path(&g_shared_picker) : 0;
        completion.file_filter = (unsigned int)g_shared_picker_filter;
        completion.ticks = event->ticks;
        g_shared_picker_active = 0;
        g_shared_picker_owner = 0;
        if (owner && owner->active)
            apply_app_result(owner, dispatch_app_event(owner, &completion),
                             sw, sh);
        invalidate_full(sw, sh);
        return 1;
    }
    if (result == GUI_FILE_PICKER_RESULT_REDRAW) {
        invalidate_full(sw, sh);
        return 1;
    }
    return 1;
}

static void sync_window_focus(int sw, int sh) {
    window_t* top = topmost();
    int next = top ? win_index(top) : -1;
    gui_app_event_t event;
    unsigned int result;
    if (next == g_focused_window) return;
    memset(&event, 0, sizeof(event));
    event.ticks = sys_get_ticks();
    if (g_focused_window >= 0 && g_focused_window < MAX_WINDOWS &&
        g_wins[g_focused_window].active) {
        window_t* old = &g_wins[g_focused_window];
        event.type = GUI_APP_EVENT_FOCUS_LOST;
        event.width = old->w;
        event.height = old->h - TITLE_H;
        result = dispatch_app_event(old, &event);
        if (result & GUI_APP_RESULT_REDRAW) invalidate_window(sw, sh, old);
    }
    g_focused_window = next;
    if (next >= 0 && g_wins[next].active) {
        window_t* focused = &g_wins[next];
        event.type = GUI_APP_EVENT_FOCUS_GAINED;
        event.width = focused->w;
        event.height = focused->h - TITLE_H;
        result = dispatch_app_event(focused, &event);
        if (result & GUI_APP_RESULT_REDRAW)
            invalidate_window(sw, sh, focused);
    }
}

static void draw_window(gfx_surface_t* s, window_t* w, int focused, int mx, int my) {
    const window_app_ops_t* ops = window_app_ops(w->type);
    gui_app_context_t context;
    /* drop shadow */
    fillr(s, w->x + 3, w->y + w->h, w->w, 3, COL_SHADOW);
    fillr(s, w->x + w->w, w->y + 3, 3, w->h, COL_SHADOW);

    fillr(s, w->x, w->y, w->w, w->h, COL_WIN_BG);
    draw_title_bar(s, w, focused,
                   w->title[0] ? w->title : (ops ? ops->title : ""));
    context.window = w;
    context.state = w->state;
    if (ops && ops->draw) ops->draw(s, &context, mx, my);

    /* Bottom-right resize grip. */
    for (int i = 0; i < 3; i++) {
        int inset = 2 + i * 3;
        hline(s, w->x + w->w - inset - 2, w->y + w->h - 2,
              inset, COL_SHADOW);
    }
}

/* ---------------- desktop icons ---------------- */

typedef struct {
    int x, y;
    const char* label;
    void (*draw)(gfx_surface_t* s, int x, int y);
    void (*action)(int sw, int sh);
} icon_t;

static void icon_files(gfx_surface_t* s, int x, int y) {
    /* folder */
    rect(s, x, y + 6, 28, 22, COL_FRAME);
    hline(s, x + 2, y + 2, 12, COL_FRAME);
    vline(s, x, y + 2, 4, COL_FRAME);
    vline(s, x + 14, y + 2, 4, COL_FRAME);
    hline(s, x + 2, y + 6, 12, COL_FRAME);
    fillr(s, x + 1, y + 7, 26, 20, 0x00FFFFE0u);
    hline(s, x + 4, y + 12, 20, COL_FRAME);
    hline(s, x + 4, y + 16, 20, COL_FRAME);
    hline(s, x + 4, y + 20, 14, COL_FRAME);
}
static void icon_system(gfx_surface_t* s, int x, int y) {
    /* monitor */
    rect(s, x, y, 28, 22, COL_FRAME);
    fillr(s, x + 2, y + 2, 24, 16, 0x0000007Fu);
    fillr(s, x + 4, y + 6, 6, 1, 0x0080C0FFu);
    fillr(s, x + 4, y + 10, 12, 1, 0x0080C0FFu);
    fillr(s, x + 4, y + 14, 8, 1, 0x0080C0FFu);
    fillr(s, x + 10, y + 22, 8, 3, COL_FRAME);
    fillr(s, x + 4, y + 25, 20, 2, COL_FRAME);
}
static void icon_config(gfx_surface_t* s, int x, int y) {
    rect(s, x + 1, y + 1, 26, 24, COL_FRAME);
    fillr(s, x + 3, y + 3, 22, 20, 0x00F0F0F0u);
    hline(s, x + 6, y + 8, 16, COL_FRAME);
    hline(s, x + 6, y + 14, 16, COL_FRAME);
    hline(s, x + 6, y + 20, 16, COL_FRAME);
    fillr(s, x + 10, y + 6, 4, 5, COL_FRAME);
    fillr(s, x + 17, y + 12, 4, 5, COL_FRAME);
    fillr(s, x + 8, y + 18, 4, 5, COL_FRAME);
}
static void icon_about(gfx_surface_t* s, int x, int y) {
    /* round-ish info bubble */
    rect(s, x + 4, y, 20, 24, COL_FRAME);
    draw_text(s, x + 11, y + 4, "i", COL_FRAME);
    draw_text(s, x + 9, y + 14, "?", COL_FRAME);
}
static void icon_terminal(gfx_surface_t* s, int x, int y) {
    rect(s, x, y, 28, 22, COL_FRAME);
    fillr(s, x + 2, y + 2, 24, 18, 0x00000000u);
    /* fake prompt + cursor inside */
    fillr(s, x + 4, y + 6, 2, 1, 0x00C8C8C8u);
    fillr(s, x + 7, y + 6, 1, 1, 0x00C8C8C8u);
    fillr(s, x + 9, y + 6, 1, 1, 0x00C8C8C8u);
    fillr(s, x + 4, y + 10, 6, 1, 0x00C8C8C8u);
    fillr(s, x + 12, y + 10, 2, 3, 0x00C8C8C8u);
    fillr(s, x + 10, y + 22, 8, 3, COL_FRAME);
    fillr(s, x + 4, y + 25, 20, 2, COL_FRAME);
}

static void icon_quit(gfx_surface_t* s, int x, int y) {
    rect(s, x + 2, y + 2, 24, 24, COL_FRAME);
    /* big X */
    for (int i = 0; i < 16; i++) {
        gui_put_pixel(s, x + 6 + i, y + 6 + i, COL_FRAME);
        gui_put_pixel(s, x + 6 + i + 1, y + 6 + i, COL_FRAME);
        gui_put_pixel(s, x + 6 + i, y + 21 - i, COL_FRAME);
        gui_put_pixel(s, x + 6 + i + 1, y + 21 - i, COL_FRAME);
    }
}

static int g_should_quit = 0;
static void show_built_window(int sw, int sh, window_t* w);

static window_t* build_window(win_type_t type,
                              int sw,
                              int sh,
                              int w,
                              int h,
                              int x,
                              int y,
                              const char* argument) {
    const window_app_ops_t* ops = window_app_ops(type);
    window_t* win = alloc_window();
    if (!win) return 0;
    if (!ops) {
        z_remove_idx(win_index(win));
        win->active = 0;
        return 0;
    }
    win->type = type;
    u_strcpy_n(win->title, ops->title, sizeof(win->title));
    win->focused_widget = 0;
    win->pressed_widget = 0;
    win->next_tick = ops->tick_interval
                   ? sys_get_ticks() + ops->tick_interval : 0u;
    if (ops->state_size) {
        win->state = malloc(ops->state_size);
        if (win->state) memset(win->state, 0, ops->state_size);
    }
    if (ops->state_size && !win->state) {
        z_remove_idx(win_index(win));
        win->active = 0;
        return 0;
    }
    win->w = w < ops->min_width ? ops->min_width : w;
    win->h = h < ops->min_height ? ops->min_height : h;
    win->x = x;
    win->y = y;
    if (win->x < 4) win->x = 4;
    if (win->y < 20) win->y = 20;
    if (win->x > sw - 32) win->x = sw - 32;
    if (win->y > sh - TASKBAR_H - TITLE_H) win->y = sh - TASKBAR_H - TITLE_H;
    win->restore_x = win->x;
    win->restore_y = win->y;
    win->restore_w = win->w;
    win->restore_h = win->h;
    if (ops->open) {
        gui_app_context_t context = {win, win->state};
        ops->open(&context, argument);
    }
    return win;
}

gui_window_t* gui_open_app(gui_app_id_t id, const char* argument) {
    const window_app_ops_t* ops = window_app_ops(id);
    window_t* window;
    int width;
    int height;
    if (!ops || g_screen_width <= 0 || g_screen_height <= 0) return 0;
    width = ops->default_width;
    height = ops->default_height;
    window = build_window(id, g_screen_width, g_screen_height,
                          width, height,
                          (g_screen_width - width) / 2,
                          (g_screen_height - TASKBAR_H - height) / 2,
                          argument);
    if (window) show_built_window(g_screen_width, g_screen_height, window);
    return window;
}

void gui_window_request_close(gui_window_t* window) {
    if (window) (void)request_window_close(window,
                                           g_screen_width,
                                           g_screen_height);
}

static void show_built_window(int sw, int sh, window_t* w) {
    invalidate_window(sw, sh, w);
    invalidate_topbar(sw, sh);
}

static void action_files(int sw, int sh) {
    window_t* w = build_window(WT_FILES, sw, sh, 360, 240,
                               (sw - 360) / 2,
                               (sh - 240) / 2, 0);
    if (!w) return;
    show_built_window(sw, sh, w);
}
static void action_system(int sw, int sh) {
    window_t* w = build_window(WT_SYSTEM, sw, sh, 280, 200,
                               (sw - 280) / 2 + 40,
                               (sh - 200) / 2 - 20, 0);
    if (!w) return;
    show_built_window(sw, sh, w);
}
static void action_config(int sw, int sh) {
    window_t* w = build_window(WT_CONFIG, sw, sh, 260, 116,
                               (sw - 260) / 2 + 20,
                               (sh - 116) / 2 + 10, 0);
    if (!w) return;
    show_built_window(sw, sh, w);
}
static void action_about(int sw, int sh) {
    window_t* w = build_window(WT_ABOUT, sw, sh, 280, 140,
                               (sw - 280) / 2 - 40,
                               (sh - 140) / 2 + 20, 0);
    if (!w) return;
    show_built_window(sw, sh, w);
}
static void action_shell(int sw, int sh) {
    window_t* w = build_window(WT_SHELL, sw, sh, 500, 320,
                               (sw - 500) / 2,
                               (sh - 320) / 2, 0);
    if (!w) return;
    /* Shell startup can touch the inherited console before PTY setup settles. */
    invalidate_full(sw, sh);
    show_built_window(sw, sh, w);
}

static void action_editor(int sw, int sh, const char* path) {
    const window_app_ops_t* ops = window_app_ops(WT_EDITOR);
    int width = ops->default_width;
    int height = ops->default_height;
    window_t* w = build_window(WT_EDITOR, sw, sh, width, height,
                               (sw - width) / 2,
                               (sh - height) / 2, path);
    if (!w) return;
    show_built_window(sw, sh, w);
}

static void action_editor_blank(int sw, int sh) {
    action_editor(sw, sh, 0);
}

static void action_viewer(int sw, int sh, const char* path) {
    const window_app_ops_t* ops = window_app_ops(GUI_APP_VIEWER);
    window_t* w = build_window(GUI_APP_VIEWER, sw, sh,
                               ops->default_width, ops->default_height,
                               (sw - ops->default_width) / 2,
                               (sh - ops->default_height) / 2, path);
    if (w) show_built_window(sw, sh, w);
}

static void action_viewer_blank(int sw, int sh) { action_viewer(sw, sh, 0); }

static void action_tasks(int sw, int sh) {
    const window_app_ops_t* ops = window_app_ops(GUI_APP_TASKS);
    window_t* w = build_window(GUI_APP_TASKS, sw, sh,
                               ops->default_width, ops->default_height,
                               (sw - ops->default_width) / 2,
                               (sh - ops->default_height) / 2, 0);
    if (w) show_built_window(sw, sh, w);
}

static void action_network(int sw, int sh) {
    const window_app_ops_t* ops = window_app_ops(GUI_APP_NETWORK);
    window_t* w = build_window(GUI_APP_NETWORK, sw, sh,
                               ops->default_width, ops->default_height,
                               (sw - ops->default_width) / 2,
                               (sh - ops->default_height) / 2, 0);
    if (w) show_built_window(sw, sh, w);
}

static void action_quit(int sw, int sh) {
    int can_quit = 1;
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        if (g_wins[i].active && !request_window_close(&g_wins[i], sw, sh))
            can_quit = 0;
    }
    if (can_quit) g_should_quit = 1;
}

static int start_entry_count(void) {
    return (int)gui_app_registry_launcher_count() + 1;
}

static const char* start_entry_label(int index) {
    const gui_app_descriptor_t* descriptor;
    if (index == (int)gui_app_registry_launcher_count()) return "Quit";
    descriptor = gui_app_registry_launcher_at((unsigned int)index);
    return descriptor ? descriptor->launcher_label : "";
}

static void start_entry_activate(int index, int sw, int sh) {
    const gui_app_descriptor_t* descriptor;
    if (index == (int)gui_app_registry_launcher_count()) {
        action_quit(sw, sh);
        return;
    }
    descriptor = gui_app_registry_launcher_at((unsigned int)index);
    if (descriptor) (void)gui_open_app(descriptor->id, 0);
}

#define ICON_COUNT 2
static icon_t g_icons[ICON_COUNT];

static void icons_layout(int sw) {
    int x = sw - 80;
    int y = 30;
    g_icons[0].x = x; g_icons[0].y = y;       g_icons[0].label = "Files";
    g_icons[0].draw = icon_files;             g_icons[0].action = action_files;
    g_icons[1].x = x; g_icons[1].y = y + 60;  g_icons[1].label = "Shell";
    g_icons[1].draw = icon_terminal;          g_icons[1].action = action_shell;
}

static int icon_hit(int mx, int my) {
    for (int i = 0; i < ICON_COUNT; i++) {
        if (mx >= g_icons[i].x && mx < g_icons[i].x + 32 &&
            my >= g_icons[i].y && my < g_icons[i].y + 32) {
            return i;
        }
    }
    return -1;
}

static void draw_icons(gfx_surface_t* s, int hover_idx) {
    for (int i = 0; i < ICON_COUNT; i++) {
        icon_t* ic = &g_icons[i];
        gui_rect_t bounds = make_rect(ic->x - 4, ic->y - 2, 42, 44);
        if (g_clip_enabled && !rect_intersects(g_clip_rect, bounds)) {
            continue;
        }
        if (i == hover_idx) {
            fillr(s, ic->x - 2, ic->y - 2, 36, 44, COL_HILIGHT);
        }
        ic->draw(s, ic->x, ic->y);
        unsigned int tw = text_width(ic->label);
        unsigned int tcolor = (i == hover_idx) ? COL_HILIGHT_T : COL_TEXT;
        draw_text(s, ic->x + 14 - (int)(tw / 2u), ic->y + 34, ic->label, tcolor);
    }
}

/* ---------------- background + desktop shell ---------------- */

static void draw_desktop(gfx_surface_t* s) {
    int x0 = 0;
    int y0 = 0;
    int x1 = (int)s->width;
    int y1 = (int)s->height;
    if (g_clip_enabled) {
        x0 = g_clip_rect.x;
        y0 = g_clip_rect.y;
        x1 = g_clip_rect.x + g_clip_rect.w;
        y1 = g_clip_rect.y + g_clip_rect.h;
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > (int)s->width) x1 = (int)s->width;
        if (y1 > (int)s->height) y1 = (int)s->height;
    }
    for (int y = y0; y < y1; y++) {
        unsigned int* row = s->pixels + y * s->pitch_pixels;
        for (int x = x0; x < x1; x++) {
            row[x] = ((x ^ y) & 1u) ? COL_DESKTOP_A : COL_DESKTOP_B;
        }
    }
}

static int active_window_count(void) {
    int count = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_wins[i].active) count++;
    return count;
}

static gui_taskbar_layout_t taskbar_layout(int sw) {
    gui_taskbar_layout_t layout;
    int count = active_window_count();
    layout = gui_taskbar_layout(sw, count);
    g_taskbar_page = gui_taskbar_clamp_page(layout, g_taskbar_page);
    return layout;
}

static gui_rect_t taskbar_window_bounds(int sw, int sh, int window_index) {
    gui_taskbar_layout_t layout = taskbar_layout(sw);
    int x = layout.first_x;
    int ordinal = 0;
    int first = g_taskbar_page * layout.per_page;
    int last = first + layout.per_page;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (!g_wins[i].active) continue;
        if (ordinal >= first && ordinal < last) {
            if (i == window_index)
                return make_rect(x, sh - TASKBAR_H + 3,
                                 layout.button_width - 3,
                                 TASKBAR_H - 6);
            x += layout.button_width;
        }
        ordinal++;
    }
    return make_rect(0, 0, 0, 0);
}

static gui_rect_t start_menu_bounds(int sh) {
    int count = start_entry_count();
    return make_rect(2,
                     sh - TASKBAR_H - count * START_MENU_ROW_H - 4,
                     START_MENU_W,
                     count * START_MENU_ROW_H + 4);
}

static void draw_start_menu(gfx_surface_t* s) {
    gui_rect_t menu;
    if (!g_start_open) return;
    menu = start_menu_bounds((int)s->height);
    fillr(s, menu.x, menu.y, menu.w, menu.h, COL_BAR);
    rect(s, menu.x, menu.y, menu.w, menu.h, COL_FRAME);
    for (int i = 0; i < start_entry_count(); i++) {
        int y = menu.y + 2 + i * START_MENU_ROW_H;
        if (i == g_start_selection) {
            fillr(s, menu.x + 2, y, menu.w - 4, START_MENU_ROW_H,
                  COL_HILIGHT);
        }
        draw_text(s, menu.x + 10, y + 6, start_entry_label(i),
                  i == g_start_selection ? COL_HILIGHT_T : COL_TEXT);
    }
}

static void draw_taskbar(gfx_surface_t* s) {
    int sw = (int)s->width;
    int sh = (int)s->height;
    int y = sh - TASKBAR_H;
    gui_taskbar_layout_t layout = taskbar_layout(sw);
    int x = layout.first_x;
    int ordinal = 0;
    int first = g_taskbar_page * layout.per_page;
    int last = first + layout.per_page;
    char status[96];
    char num[16];
    char clock_text[8] = "--:--";
    sys_fsinfo_t fs;
    struct timespec ts;
    struct tm tm;
    window_t* focused = topmost();

    fillr(s, 0, y, sw, TASKBAR_H, COL_BAR);
    hline(s, 0, y, sw, COL_FRAME);
    gui_widget_button(s, make_rect(4, y + 3, 50, TASKBAR_H - 6), "Start",
                      (gui_widget_state_t){0, g_start_open, g_start_open, 0},
                      &GUI_WIDGET_THEME, draw_text);

    if (layout.paging) {
        gui_widget_button(s, make_rect(58, y + 3, 17, TASKBAR_H - 6), "<",
                          (gui_widget_state_t){0, 0, 0, g_taskbar_page == 0},
                          &GUI_WIDGET_THEME, draw_text);
        gui_widget_button(s, make_rect(sw - 207, y + 3, 17, TASKBAR_H - 6), ">",
                          (gui_widget_state_t){0, 0, 0,
                              g_taskbar_page + 1 >= layout.page_count},
                          &GUI_WIDGET_THEME, draw_text);
    }

    for (int i = 0; i < MAX_WINDOWS; i++) {
        gui_widget_state_t state;
        if (!g_wins[i].active) continue;
        if (ordinal < first || ordinal >= last) {
            ordinal++;
            continue;
        }
        state.hovered = 0;
        state.pressed = 0;
        state.focused = &g_wins[i] == focused && !g_wins[i].minimized;
        state.disabled = 0;
        gui_widget_button(s, make_rect(x, y + 3,
                                      layout.button_width - 3,
                                      TASKBAR_H - 6),
                          g_wins[i].title, state, &GUI_WIDGET_THEME, draw_text);
        x += layout.button_width;
        ordinal++;
    }

    u_strcpy_n(status, "Free ", sizeof(status));
    if (sys_fsinfo(&fs) == 0) {
        utoa10(fs.free_bytes / 1024u, num);
        u_strcat_n(status, num, sizeof(status));
        u_strcat_n(status, "K", sizeof(status));
    } else {
        u_strcat_n(status, "?", sizeof(status));
    }
    if (g_perf_visible) {
        u_strcat_n(status, " P", sizeof(status));
        utoa10(g_perf.shown_presents, num);
        u_strcat_n(status, num, sizeof(status));
    }
    if (clock_gettime(CLOCK_REALTIME, &ts) == 0 && gmtime_r(&ts.tv_sec, &tm)) {
        clock_text[0] = (char)('0' + (tm.tm_hour / 10));
        clock_text[1] = (char)('0' + (tm.tm_hour % 10));
        clock_text[3] = (char)('0' + (tm.tm_min / 10));
        clock_text[4] = (char)('0' + (tm.tm_min % 10));
    }
    draw_text(s, sw - 184, y + 9, status, COL_TEXT);
    draw_text(s, sw - 40, y + 9, clock_text, COL_TEXT);

    if (g_desktop_notice[0] &&
        !tick_due(sys_get_ticks(), g_desktop_notice_until)) {
        int notice_w = (int)text_width(g_desktop_notice) + 12;
        fillr(s, sw - notice_w - 4, y - 20, notice_w, 18, COL_WIN_BG);
        rect(s, sw - notice_w - 4, y - 20, notice_w, 18, COL_FRAME);
        draw_text(s, sw - notice_w + 2, y - 14, g_desktop_notice, COL_TEXT);
    }
}

/* ---------------- compose ---------------- */

static void compose_v2(gfx_surface_t* s, int mx, int my) {
    g_perf.composed_pixels += s->width * s->height;
    g_perf.total_composed_pixels += s->width * s->height;
    clip_clear();
    draw_desktop(s);

    int hi = icon_hit(mx, my);
    if (hit_window_z(mx, my)) hi = -1;
    draw_icons(s, hi);

    window_t* top = topmost();
    for (int i = 0; i < gui_window_stack_count(&g_window_stack); i++) {
        window_t* w = &g_wins[gui_window_stack_at(&g_window_stack, i)];
        if (!w->active || w->minimized) continue;
        draw_window(s, w, w == top, mx, my);
    }
    draw_start_menu(s);
    if (g_shared_picker_active)
        gui_file_picker_draw(s, &g_shared_picker, shared_picker_bounds(),
                             &GUI_WIDGET_THEME, &SHARED_PICKER_STYLE,
                             draw_text);
    draw_taskbar(s);
}

static int present_cursor_rect(gfx_context_t* gfx, int mx, int my, int draw);

static unsigned int compose_rect(gfx_surface_t* s, gui_rect_t r,
                                 int mx, int my) {
    gui_rect_t opaque[GUI_WINDOW_CAPACITY + 3];
    gui_rect_t visible[GUI_VISIBLE_REGION_CAPACITY];
    int window_count = gui_window_stack_count(&g_window_stack);
    unsigned int pixels = 0;
    int opaque_count = 0;
    int visible_count;
    int hi = icon_hit(mx, my);
    window_t* top = topmost();

    if (hit_window_z(mx, my)) hi = -1;
    if (g_shared_picker_active)
        opaque[opaque_count++] = shared_picker_bounds();
    if (g_start_open) opaque[opaque_count++] = start_menu_bounds((int)s->height);
    opaque[opaque_count++] = make_rect(0, (int)s->height - TASKBAR_H,
                                       (int)s->width, TASKBAR_H);
    for (int i = window_count - 1; i >= 0; i--) {
        window_t* w = &g_wins[gui_window_stack_at(&g_window_stack, i)];
        if (w->active && !w->minimized)
            opaque[opaque_count++] = make_rect(w->x, w->y, w->w, w->h);
    }
    visible_count = gui_visible_regions(r, opaque, opaque_count,
                                        visible, GUI_VISIBLE_REGION_CAPACITY);
    if (visible_count < 0) visible_count = 1, visible[0] = r;
    for (int p = 0; p < visible_count; p++) {
        clip_set(visible[p]);
        draw_desktop(s);
        draw_icons(s, hi);
        pixels += (unsigned int)visible[p].w * (unsigned int)visible[p].h;
    }

    for (int i = 0; i < window_count; i++) {
        window_t* w = &g_wins[gui_window_stack_at(&g_window_stack, i)];
        opaque_count = 0;
        if (!w->active || w->minimized ||
            !rect_intersects(r, window_screen_rect(w))) continue;
        if (g_shared_picker_active)
            opaque[opaque_count++] = shared_picker_bounds();
        if (g_start_open)
            opaque[opaque_count++] = start_menu_bounds((int)s->height);
        opaque[opaque_count++] = make_rect(0, (int)s->height - TASKBAR_H,
                                           (int)s->width, TASKBAR_H);
        for (int above = window_count - 1; above > i; above--) {
            window_t* covering =
                &g_wins[gui_window_stack_at(&g_window_stack, above)];
            if (covering->active && !covering->minimized)
                opaque[opaque_count++] = make_rect(covering->x, covering->y,
                                                   covering->w, covering->h);
        }
        visible_count = gui_visible_regions(r, opaque, opaque_count,
                                            visible, GUI_VISIBLE_REGION_CAPACITY);
        if (visible_count < 0) visible_count = 1, visible[0] = r;
        for (int p = 0; p < visible_count; p++) {
            if (!rect_intersects(visible[p], window_screen_rect(w))) continue;
            clip_set(visible[p]);
            draw_window(s, w, w == top, mx, my);
            pixels += (unsigned int)visible[p].w * (unsigned int)visible[p].h;
        }
    }
    clip_set(r);
    if (g_start_open && rect_intersects(r, start_menu_bounds((int)s->height))) {
        draw_start_menu(s);
        pixels += (unsigned int)r.w * (unsigned int)r.h;
    }
    if (g_shared_picker_active &&
        rect_intersects(r, shared_picker_bounds())) {
        gui_file_picker_draw(s, &g_shared_picker, shared_picker_bounds(),
                             &GUI_WIDGET_THEME, &SHARED_PICKER_STYLE,
                             draw_text);
        pixels += (unsigned int)r.w * (unsigned int)r.h;
    }
    if (rect_intersects(r, make_rect(0, (int)s->height - TASKBAR_H,
                                     (int)s->width, TASKBAR_H))) {
        draw_taskbar(s);
        pixels += (unsigned int)r.w * (unsigned int)r.h;
    }
    clip_clear();
    return pixels;
}

static int present_dirty_scene(gfx_context_t* gfx,
                               int mx,
                               int my,
                               int cursor_mx,
                               int cursor_my) {
    gui_rect_t cursor = cursor_screen_rect(cursor_mx, cursor_my);
    int sw = (int)gfx->backbuffer.width;
    int sh = (int)gfx->backbuffer.height;
    int cursor_touched = 0;

    for (int i = 0; i < g_dirty_count; i++) {
        gui_rect_t r = rect_clip_screen(g_dirty[i], sw, sh);
        gui_rect_t pieces[4];
        int piece_count;
        if (rect_empty(r)) continue;
        unsigned int composed;
        g_perf.dirty_regions++;
        g_perf.total_dirty_regions++;
        composed = compose_rect(&gfx->backbuffer, r, mx, my);
        g_perf.composed_pixels += composed;
        g_perf.total_composed_pixels += composed;

        cursor_touched |= rect_intersects(r, cursor);
        piece_count = gui_rect_exclude(r, cursor, pieces);
        for (int piece = 0; piece < piece_count; piece++) {
            gui_rect_t p = pieces[piece];
            uint32_t present_start = sys_get_ticks();
            if (gfx_present_rect(gfx, (unsigned int)p.x, (unsigned int)p.y,
                                 (unsigned int)p.w, (unsigned int)p.h) < 0) {
                return -1;
            }
            perf_note_present(present_start);
            g_perf.presented_pixels += (unsigned int)p.w * (unsigned int)p.h;
            g_perf.total_presented_pixels +=
                (unsigned int)p.w * (unsigned int)p.h;
        }
    }
    if (cursor_touched) {
        if (present_cursor_rect(gfx, cursor_mx, cursor_my, 1) < 0) {
            return -1;
        }
    }
    dirty_clear();
    return 0;
}

static int present_cursor_rect(gfx_context_t* gfx, int mx, int my, int draw) {
    unsigned int tmp[CURSOR_W * CURSOR_H];
    gfx_surface_t out;
    gfx_surface_t* scene;
    int w = CURSOR_W;
    int h = CURSOR_H;

    if (!gfx) return -1;
    scene = &gfx->backbuffer;
    if (!scene || !scene->pixels || mx < 0 || my < 0 ||
        mx >= (int)scene->width || my >= (int)scene->height) {
        return 0;
    }

    if (mx + w > (int)scene->width) w = (int)scene->width - mx;
    if (my + h > (int)scene->height) h = (int)scene->height - my;
    if (w <= 0 || h <= 0) return 0;

    for (int y = 0; y < h; y++) {
        unsigned int* src = scene->pixels + (my + y) * scene->pitch_pixels + mx;
        for (int x = 0; x < w; x++) {
            tmp[y * w + x] = src[x];
        }
    }

    out.width = (unsigned int)w;
    out.height = (unsigned int)h;
    out.pitch_pixels = (unsigned int)w;
    out.pixels = tmp;
    if (draw) {
        draw_cursor(&out, 0, 0);
    }

    {
        uint32_t present_start = sys_get_ticks();
        int rc = gfx_present_surface(gfx, (unsigned int)mx,
                                     (unsigned int)my, &out);
        if (rc >= 0) perf_note_present(present_start);
        return rc;
    }
}

static int present_frame_with_cursor(gfx_context_t* gfx, int mx, int my) {
    uint32_t present_start = sys_get_ticks();

    if (gfx_present(gfx) < 0) {
        return -1;
    }
    perf_note_present(present_start);
    g_perf.presented_pixels += gfx->backbuffer.width * gfx->backbuffer.height;
    g_perf.total_presented_pixels +=
        gfx->backbuffer.width * gfx->backbuffer.height;
    if (present_cursor_rect(gfx, mx, my, 1) < 0) {
        return -1;
    }
    return 0;
}

static int present_cursor_move(gfx_context_t* gfx,
                               int old_mx,
                               int old_my,
                               int mx,
                               int my) {
    unsigned int tmp[CURSOR_MOVE_MAX_W * CURSOR_MOVE_MAX_H];
    gfx_surface_t out;
    gfx_surface_t* scene;
    int x0;
    int y0;
    int x1;
    int y1;
    int w;
    int h;

    if (old_mx == mx && old_my == my) return 0;
    scene = &gfx->backbuffer;

    x0 = old_mx < mx ? old_mx : mx;
    y0 = old_my < my ? old_my : my;
    x1 = old_mx > mx ? old_mx + CURSOR_W : mx + CURSOR_W;
    y1 = old_my > my ? old_my + CURSOR_H : my + CURSOR_H;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)scene->width) x1 = (int)scene->width;
    if (y1 > (int)scene->height) y1 = (int)scene->height;
    w = x1 - x0;
    h = y1 - y0;

    if (w > 0 && h > 0 &&
        w <= CURSOR_MOVE_MAX_W &&
        h <= CURSOR_MOVE_MAX_H) {
        for (int y = 0; y < h; y++) {
            unsigned int* src = scene->pixels + (y0 + y) * scene->pitch_pixels + x0;
            for (int x = 0; x < w; x++) {
                tmp[y * w + x] = src[x];
            }
        }

        out.width = (unsigned int)w;
        out.height = (unsigned int)h;
        out.pitch_pixels = (unsigned int)w;
        out.pixels = tmp;
        draw_cursor(&out, mx - x0, my - y0);

        {
            uint32_t present_start = sys_get_ticks();
            int rc = gfx_present_surface(gfx, (unsigned int)x0,
                                         (unsigned int)y0, &out);
            if (rc >= 0) perf_note_present(present_start);
            return rc;
        }
    }

    if (present_cursor_rect(gfx, old_mx, old_my, 0) < 0) {
        return -1;
    }
    if (present_cursor_rect(gfx, mx, my, 1) < 0) {
        return -1;
    }
    return 0;
}

static void drag_preview_strips(gui_rect_t r, gui_rect_t strips[4]) {
    strips[0] = make_rect(r.x - 1, r.y - 1, r.w + 2, 3);
    strips[1] = make_rect(r.x - 1, r.y + r.h - 2, r.w + 2, 3);
    strips[2] = make_rect(r.x - 1, r.y, 3, r.h);
    strips[3] = make_rect(r.x + r.w - 2, r.y, 3, r.h);
}

static void draw_drag_outline(gfx_surface_t* s,
                              gui_rect_t surface_rect,
                              gui_rect_t outline_rect) {
    int x = outline_rect.x - surface_rect.x;
    int y = outline_rect.y - surface_rect.y;

    rect(s, x, y, outline_rect.w, outline_rect.h, 0x00FFFFFFu);
    if (outline_rect.w > 2 && outline_rect.h > 2) {
        rect(s, x + 1, y + 1, outline_rect.w - 2, outline_rect.h - 2,
             0x00000000u);
    }
}

static int present_cursor_rect_with_drag_overlay(gfx_context_t* gfx,
                                                 int mx,
                                                 int my,
                                                 int draw) {
    unsigned int tmp[CURSOR_W * CURSOR_H];
    gfx_surface_t out;
    gfx_surface_t* scene;
    gui_rect_t r;
    int w = CURSOR_W;
    int h = CURSOR_H;

    if (!gfx) return -1;
    scene = &gfx->backbuffer;
    if (!scene->pixels || mx < 0 || my < 0 ||
        mx >= (int)scene->width || my >= (int)scene->height) {
        return 0;
    }

    if (mx + w > (int)scene->width) w = (int)scene->width - mx;
    if (my + h > (int)scene->height) h = (int)scene->height - my;
    if (w <= 0 || h <= 0) return 0;
    r = make_rect(mx, my, w, h);

    for (int y = 0; y < h; y++) {
        unsigned int* src = scene->pixels + (my + y) * scene->pitch_pixels + mx;
        for (int x = 0; x < w; x++) {
            tmp[y * w + x] = src[x];
        }
    }

    out.width = (unsigned int)w;
    out.height = (unsigned int)h;
    out.pitch_pixels = (unsigned int)w;
    out.pixels = tmp;
    if (g_drag_overlay_visible) {
        draw_drag_outline(&out, r, drag_overlay_rect());
    }
    if (draw) {
        draw_cursor(&out, 0, 0);
    }

    {
        uint32_t present_start = sys_get_ticks();
        int rc = gfx_present_surface(gfx, (unsigned int)mx,
                                     (unsigned int)my, &out);
        if (rc >= 0) perf_note_present(present_start);
        return rc;
    }
}

static int present_cursor_move_with_drag_overlay(gfx_context_t* gfx,
                                                 int old_mx,
                                                 int old_my,
                                                 int mx,
                                                 int my) {
    unsigned int tmp[CURSOR_MOVE_MAX_W * CURSOR_MOVE_MAX_H];
    gfx_surface_t out;
    gfx_surface_t* scene;
    gui_rect_t r;
    int x0;
    int y0;
    int x1;
    int y1;
    int w;
    int h;

    if (old_mx == mx && old_my == my) return 0;
    if (!gfx) return -1;
    scene = &gfx->backbuffer;
    if (!scene->pixels) return -1;

    x0 = old_mx < mx ? old_mx : mx;
    y0 = old_my < my ? old_my : my;
    x1 = old_mx > mx ? old_mx + CURSOR_W : mx + CURSOR_W;
    y1 = old_my > my ? old_my + CURSOR_H : my + CURSOR_H;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)scene->width) x1 = (int)scene->width;
    if (y1 > (int)scene->height) y1 = (int)scene->height;
    w = x1 - x0;
    h = y1 - y0;

    if (w <= 0 || h <= 0) return 0;
    if (w > CURSOR_MOVE_MAX_W || h > CURSOR_MOVE_MAX_H) {
        if (present_cursor_rect_with_drag_overlay(gfx, old_mx, old_my, 0) < 0) {
            return -1;
        }
        return present_cursor_rect_with_drag_overlay(gfx, mx, my, 1);
    }
    r = make_rect(x0, y0, w, h);

    for (int y = 0; y < h; y++) {
        unsigned int* src = scene->pixels + (y0 + y) * scene->pitch_pixels + x0;
        for (int x = 0; x < w; x++) {
            tmp[y * w + x] = src[x];
        }
    }

    out.width = (unsigned int)w;
    out.height = (unsigned int)h;
    out.pitch_pixels = (unsigned int)w;
    out.pixels = tmp;
    if (g_drag_overlay_visible) {
        draw_drag_outline(&out, r, drag_overlay_rect());
    }
    draw_cursor(&out, mx - x0, my - y0);

    {
        uint32_t present_start = sys_get_ticks();
        int rc = gfx_present_surface(gfx, (unsigned int)x0,
                                     (unsigned int)y0, &out);
        if (rc >= 0) perf_note_present(present_start);
        return rc;
    }
}

static int drag_preview_dirty_add(gui_rect_t* rects,
                                  int* count,
                                  int max_count,
                                  int sw,
                                  int sh,
                                  gui_rect_t r) {
    r = rect_clip_screen(r, sw, sh);
    if (rect_empty(r)) return 0;

    for (int i = 0; i < *count; i++) {
        if (rect_should_merge(rects[i], r)) {
            rects[i] = rect_clip_screen(rect_union(rects[i], r), sw, sh);
            return 0;
        }
    }

    if (*count >= max_count) {
        rects[0] = rect_union(rects[0], r);
        rects[0] = rect_clip_screen(rects[0], sw, sh);
        *count = 1;
        return 0;
    }

    rects[*count] = r;
    *count = *count + 1;
    return 0;
}

static int present_drag_preview_rect(gfx_context_t* gfx,
                                     gui_rect_t r,
                                     gui_rect_t outline,
                                     int draw) {
    unsigned int* tmp;
    gfx_surface_t out;
    int sw;
    int sh;

    if (!gfx || !gfx->backbuffer.pixels) return -1;
    sw = (int)gfx->backbuffer.width;
    sh = (int)gfx->backbuffer.height;
    r = rect_clip_screen(r, sw, sh);
    if (rect_empty(r)) return 0;
    {
        unsigned int needed = (unsigned int)r.w * (unsigned int)r.h;
        unsigned int available = gfx->scratch.width * gfx->scratch.height;
        if (!gfx->scratch.pixels || available < needed) {
            gfx_surface_free(&gfx->scratch);
            if (!gfx_surface_alloc(&gfx->scratch, needed, 1u)) return -1;
        }
    }
    tmp = gfx->scratch.pixels;

    for (int y = 0; y < r.h; y++) {
        unsigned int* src = gfx->backbuffer.pixels +
            (unsigned int)(r.y + y) * gfx->backbuffer.pitch_pixels +
            (unsigned int)r.x;
        for (int x = 0; x < r.w; x++) {
            tmp[y * r.w + x] = src[x];
        }
    }

    out.width = (unsigned int)r.w;
    out.height = (unsigned int)r.h;
    out.pitch_pixels = (unsigned int)r.w;
    out.pixels = tmp;
    if (draw) {
        draw_drag_outline(&out, r, outline);
    }

    {
        uint32_t present_start = sys_get_ticks();
        int rc = gfx_present_surface(gfx, (unsigned int)r.x,
                                     (unsigned int)r.y, &out);
        if (rc >= 0) perf_note_present(present_start);
        return rc;
    }
}

static int present_drag_preview(gfx_context_t* gfx, gui_rect_t r, int draw) {
    gui_rect_t strips[4];

    drag_preview_strips(r, strips);
    for (int i = 0; i < 4; i++) {
        if (present_drag_preview_rect(gfx, strips[i], r, draw) < 0) return -1;
    }
    return 0;
}

static int present_drag_preview_move(gfx_context_t* gfx,
                                     gui_rect_t old_rect,
                                     gui_rect_t new_rect) {
    gui_rect_t strips[4];
    gui_rect_t dirty[8];
    int dirty_count = 0;
    int sw;
    int sh;

    if (!gfx) return -1;
    sw = (int)gfx->backbuffer.width;
    sh = (int)gfx->backbuffer.height;

    drag_preview_strips(old_rect, strips);
    for (int i = 0; i < 4; i++) {
        drag_preview_dirty_add(dirty, &dirty_count, 8, sw, sh, strips[i]);
    }
    drag_preview_strips(new_rect, strips);
    for (int i = 0; i < 4; i++) {
        drag_preview_dirty_add(dirty, &dirty_count, 8, sw, sh, strips[i]);
    }

    for (int i = 0; i < dirty_count; i++) {
        if (present_drag_preview_rect(gfx, dirty[i], new_rect, 1) < 0) {
            return -1;
        }
    }
    return 0;
}

static int present_drag_frame(gfx_context_t* gfx,
                              int mx,
                              int my,
                              uint32_t now) {
    gui_rect_t old_overlay;
    gui_rect_t new_overlay;

    if (g_drag != DRAG_MOVE || g_drag_idx < 0) return 0;

    old_overlay = drag_overlay_rect();
    new_overlay = drag_preview_rect();
    if (g_drag_overlay_visible) {
        if ((old_overlay.x != new_overlay.x || old_overlay.y != new_overlay.y) &&
            present_drag_preview_move(gfx, old_overlay, new_overlay) < 0) {
            return -1;
        }
    } else if (present_drag_preview(gfx, new_overlay, 1) < 0) {
        return -1;
    }

    g_drag_overlay_visible = 1;
    g_drag_overlay_x = g_drag_preview_x;
    g_drag_overlay_y = g_drag_preview_y;

    return present_cursor_rect_with_drag_overlay(gfx, mx, my, 1);
}

static int present_gui_frame(gfx_context_t* gfx,
                             int mx,
                             int my,
                             uint32_t now) {
    int did_work = 0;

    if (drag_target_pending()) {
        if (present_drag_frame(gfx, mx, my, now) < 0) {
            return -1;
        }
        did_work = 1;
    }

    if (did_work) {
        gui_finish_frame(now);
    } else {
        gui_cancel_frame();
    }
    return 0;
}

/* ---------------- input handling ---------------- */

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static int absi(int v) {
    return v < 0 ? -v : v;
}

static gui_fp_t fp_from_int(int v) {
    return (gui_fp_t)(v * GUI_FP_ONE);
}

static int fp_to_int_round(gui_fp_t v) {
    if (v >= 0) return (v + GUI_FP_ONE / 2) >> GUI_FP_SHIFT;
    return -(((-v) + GUI_FP_ONE / 2) >> GUI_FP_SHIFT);
}

static gui_fp_t fp_clamp_int(gui_fp_t v, int lo, int hi) {
    gui_fp_t flo = fp_from_int(lo);
    gui_fp_t fhi = fp_from_int(hi);

    if (v < flo) return flo;
    if (v > fhi) return fhi;
    return v;
}

static gui_fp_t abs_to_screen_fp(unsigned int value, int max_value) {
    unsigned long long scaled;

    if (max_value <= 0) return 0;
    if (value > 65535u) value = 65535u;
    /*
     * Keep absolute-device transforms in subpixel space. Drawing still lands on
     * integer pixels, but cursor/window math should not discard transform
     * precision before the final presentation step.
     */
    scaled = (unsigned long long)value *
             (unsigned long long)(unsigned int)max_value *
             (unsigned long long)GUI_FP_ONE;
    return (gui_fp_t)((scaled + 32767ull) / 65535ull);
}

static int point_in(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static int mouse_event_mergeable(const sys_input_event_t* a,
                                 const sys_input_event_t* b) {
    int dx;
    int dy;

    if (!a || !b) return 0;
    if (a->type != SYS_INPUT_TYPE_MOUSE || b->type != SYS_INPUT_TYPE_MOUSE) return 0;
    if ((a->flags & SYS_INPUT_MOUSE_ABSOLUTE) != (b->flags & SYS_INPUT_MOUSE_ABSOLUTE)) return 0;
    if (a->wheel != 0 || b->wheel != 0) return 0;
    if (a->button_changes != 0u || b->button_changes != 0u) return 0;
    if (a->buttons != b->buttons) return 0;
    dx = a->dx + b->dx;
    dy = a->dy + b->dy;
    if (absi(dx) + absi(dy) > MOUSE_MERGE_MAX_DELTA) return 0;
    return 1;
}

static int read_input_coalesced(sys_input_event_t* events,
                                unsigned int cap,
                                unsigned int flags) {
    sys_input_event_t raw[MOUSE_COALESCE_MAX];
    int total = 0;
    int n;
    int should_coalesce;

    if (!events || cap == 0u) return 0;

    n = sys_input_read(raw, MOUSE_COALESCE_MAX, flags);
    if (n <= 0) return n;
    should_coalesce = n == MOUSE_COALESCE_MAX;

    if (!should_coalesce) {
        for (int i = 0; i < n && i < (int)cap; i++) {
            events[i] = raw[i];
        }
        return n < (int)cap ? n : (int)cap;
    }

    for (int i = 0; i < n && total < (int)cap; i++) {
        sys_input_event_t ev = raw[i];

        while (ev.type == SYS_INPUT_TYPE_MOUSE &&
               ev.button_changes == 0u &&
               ev.wheel == 0 &&
               i + 1 < n &&
               mouse_event_mergeable(&ev, &raw[i + 1])) {
            sys_input_event_t* next = &raw[++i];
            if (ev.flags & SYS_INPUT_MOUSE_ABSOLUTE) {
                ev.abs_x = next->abs_x;
                ev.abs_y = next->abs_y;
                ev.dx += next->dx;
                ev.dy += next->dy;
            } else {
                ev.dx += next->dx;
                ev.dy += next->dy;
            }
            ev.ticks = next->ticks;
            ev.sequence = next->sequence;
        }

        events[total++] = ev;
    }

    return total;
}

static void dispatch_window_resize(window_t* w, uint32_t ticks) {
    gui_app_event_t event;
    if (!w || !w->active) return;
    memset(&event, 0, sizeof(event));
    event.type = GUI_APP_EVENT_RESIZE;
    event.width = w->w;
    event.height = w->h - TITLE_H;
    event.ticks = ticks;
    (void)dispatch_app_event(w, &event);
}

static void window_toggle_maximize(window_t* w, int sw, int sh) {
    if (!w || !w->active) return;
    if (w->maximized) {
        w->x = w->restore_x;
        w->y = w->restore_y;
        w->w = w->restore_w;
        w->h = w->restore_h;
        w->maximized = 0;
    } else {
        w->restore_x = w->x;
        w->restore_y = w->y;
        w->restore_w = w->w;
        w->restore_h = w->h;
        w->x = 0;
        w->y = 0;
        w->w = sw;
        w->h = sh - TASKBAR_H;
        w->maximized = 1;
    }
    w->minimized = 0;
    z_push_top(win_index(w));
    dispatch_window_resize(w, sys_get_ticks());
    invalidate_full(sw, sh);
}

static void window_snap(window_t* w, int sw, int sh, int side) {
    if (!w || !w->active) return;
    if (!w->maximized) {
        w->restore_x = w->x;
        w->restore_y = w->y;
        w->restore_w = w->w;
        w->restore_h = w->h;
    }
    w->maximized = 0;
    w->x = side < 0 ? 0 : sw / 2;
    w->y = 0;
    w->w = side < 0 ? sw / 2 : sw - sw / 2;
    w->h = sh - TASKBAR_H;
    dispatch_window_resize(w, sys_get_ticks());
    invalidate_full(sw, sh);
}

static void window_minimize(window_t* w, int sw, int sh) {
    if (!w || !w->active || w->minimized) return;
    w->minimized = 1;
    invalidate_full(sw, sh);
}

static void window_restore_focus(window_t* w, int sw, int sh) {
    if (!w || !w->active) return;
    w->minimized = 0;
    z_push_top(win_index(w));
    invalidate_full(sw, sh);
}

static void cycle_windows(int sw, int sh) {
    int count = gui_window_stack_count(&g_window_stack);
    int current = -1;
    window_t* top = topmost();
    if (count <= 0) return;
    if (top) {
        for (int i = 0; i < count; i++) {
            if (gui_window_stack_at(&g_window_stack, i) == win_index(top)) {
                current = i;
                break;
            }
        }
    }
    for (int step = 1; step <= count; step++) {
        int pos = (current + step) % count;
        window_t* w = &g_wins[gui_window_stack_at(&g_window_stack, pos)];
        if (w->active) {
            window_restore_focus(w, sw, sh);
            return;
        }
    }
}

static int handle_desktop_shell_click(int mx, int my, int sw, int sh) {
    int bar_y = sh - TASKBAR_H;
    gui_rect_t start = make_rect(4, bar_y + 3, 50, TASKBAR_H - 6);

    if (g_start_open) {
        gui_rect_t menu = start_menu_bounds(sh);
        if (point_in(mx, my, menu.x, menu.y, menu.w, menu.h)) {
            int row = (my - menu.y - 2) / START_MENU_ROW_H;
            if (row >= 0 && row < start_entry_count()) {
                g_start_selection = row;
                g_start_open = 0;
                invalidate_full(sw, sh);
                start_entry_activate(row, sw, sh);
            }
            return 1;
        }
    }

    if (point_in(mx, my, start.x, start.y, start.w, start.h)) {
        g_start_open = !g_start_open;
        g_start_selection = 0;
        invalidate_full(sw, sh);
        return 1;
    }

    if (my >= bar_y) {
        gui_taskbar_layout_t layout = taskbar_layout(sw);
        if (layout.paging &&
            point_in(mx, my, 58, bar_y + 3, 17, TASKBAR_H - 6)) {
            if (g_taskbar_page > 0) g_taskbar_page--;
            invalidate_topbar(sw, sh);
            return 1;
        }
        if (layout.paging &&
            point_in(mx, my, sw - 207, bar_y + 3, 17, TASKBAR_H - 6)) {
            if (g_taskbar_page + 1 < layout.page_count) g_taskbar_page++;
            invalidate_topbar(sw, sh);
            return 1;
        }
        for (int i = 0; i < MAX_WINDOWS; i++) {
            gui_rect_t bounds;
            window_t* top;
            if (!g_wins[i].active) continue;
            bounds = taskbar_window_bounds(sw, sh, i);
            if (!point_in(mx, my, bounds.x, bounds.y, bounds.w, bounds.h))
                continue;
            top = topmost();
            if (&g_wins[i] == top && !g_wins[i].minimized)
                window_minimize(&g_wins[i], sw, sh);
            else
                window_restore_focus(&g_wins[i], sw, sh);
            g_start_open = 0;
            return 1;
        }
        if (g_start_open) {
            g_start_open = 0;
            invalidate_full(sw, sh);
        }
        return 1;
    }

    if (g_start_open) {
        g_start_open = 0;
        invalidate_full(sw, sh);
        return 1;
    }
    return 0;
}

static int handle_start_key(unsigned int key, unsigned int ascii,
                            int sw, int sh) {
    if (!g_start_open) return 0;
    if (key == KEY_ESC) {
        g_start_open = 0;
    } else if (key == KEY_UP) {
        g_start_selection = gui_start_move_selection(
            g_start_selection, start_entry_count(), -1);
    } else if (key == KEY_DOWN || key == KEY_TAB) {
        g_start_selection = gui_start_move_selection(
            g_start_selection, start_entry_count(), 1);
    } else if (key == KEY_ENTER) {
        int selection = g_start_selection;
        g_start_open = 0;
        invalidate_full(sw, sh);
        start_entry_activate(selection, sw, sh);
        return 1;
    } else if (ascii >= 'a' && ascii <= 'z') {
        const char* labels[16];
        int count = start_entry_count();
        int match;
        for (int i = 0; i < count; i++) labels[i] = start_entry_label(i);
        match = gui_start_first_letter(labels, count, (char)ascii);
        if (match >= 0) g_start_selection = match;
    } else {
        return 1;
    }
    invalidate_full(sw, sh);
    return 1;
}

static int dispatch_due_app_ticks(int sw, int sh, uint32_t now) {
    int dirty = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = &g_wins[i];
        const window_app_ops_t* ops;
        gui_app_event_t event;
        unsigned int result;
        if (!w->active) continue;
        ops = window_app_ops(w->type);
        if (w->minimized && (!ops || !ops->background_ticks)) continue;
        if (!ops || !ops->tick_interval) continue;
        if (w->next_tick == 0u) w->next_tick = now + ops->tick_interval;
        if (!tick_due(now, w->next_tick)) continue;
        memset(&event, 0, sizeof(event));
        event.type = GUI_APP_EVENT_TICK;
        event.width = w->w;
        event.height = w->h - TITLE_H;
        event.ticks = now;
        result = dispatch_app_event(w, &event);
        apply_app_result(w, result, sw, sh);
        if (((result & GUI_APP_RESULT_REDRAW) || g_dirty_count > 0) &&
            !w->minimized) dirty = 1;
        if (w->active) w->next_tick = now + ops->tick_interval;
    }
    return dirty;
}

static int poll_shell_windows(int sw, int sh) {
    int dirty = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        window_t* w = &g_wins[i];
        if (!w->active || w->type != WT_SHELL || !w->state) continue;
        if (gui_shell_poll(SHELL_STATE(w))) {
            if (!w->minimized) {
                invalidate_window(sw, sh, w);
                dirty = 1;
            }
        }
    }
    return dirty;
}

static unsigned int shell_poll_fds(struct pollfd* fds, unsigned int capacity) {
    unsigned int count = 0;
    for (int i = 0; i < MAX_WINDOWS && count < capacity; i++) {
        int fd;
        if (!g_wins[i].active || g_wins[i].type != WT_SHELL ||
            !g_wins[i].state) continue;
        fd = gui_shell_poll_fd(SHELL_STATE(&g_wins[i]));
        if (fd < 0) continue;
        fds[count].fd = fd;
        fds[count].events = POLLIN | POLLHUP | POLLERR;
        fds[count].revents = 0;
        count++;
    }
    return count;
}

static uint32_t gui_wait_deadline(uint32_t now) {
    uint32_t deadline = 0;
    int have_deadline = 0;

    if (g_clock_next_tick && !tick_due(now, g_clock_next_tick)) {
        deadline = g_clock_next_tick;
        have_deadline = 1;
    }

    if (gui_frame_pending() && !gui_frame_due(now)) {
        deadline = have_deadline
                 ? min_deadline(deadline, gui_frame_deadline())
                 : gui_frame_deadline();
        have_deadline = 1;
    }

    for (int i = 0; i < MAX_WINDOWS; i++) {
        const window_app_ops_t* ops;
        if (!g_wins[i].active) continue;
        ops = window_app_ops(g_wins[i].type);
        if (g_wins[i].minimized && (!ops || !ops->background_ticks)) continue;
        if (!ops || !ops->tick_interval) continue;
        if (g_wins[i].next_tick == 0u || tick_due(now, g_wins[i].next_tick))
            return now;
        deadline = have_deadline
                 ? min_deadline(deadline, g_wins[i].next_tick)
                 : g_wins[i].next_tick;
        have_deadline = 1;
    }

    return have_deadline ? deadline : 0u;
}

static int hover_key(int mx, int my) {
    window_t* w = hit_window_z(mx, my);

    if (!w) {
        int icon = icon_hit(mx, my);
        return icon >= 0 ? 1000 + icon : 0;
    }

    return 3000 + win_index(w);
}

static int invalidate_hover_key(int sw, int sh, int key) {
    if (key >= 1000 && key < 1000 + ICON_COUNT) {
        int idx = key - 1000;
        invalidate_rect(sw, sh, make_rect(g_icons[idx].x - 2, g_icons[idx].y - 2, 36, 44));
        return 1;
    }

    return 0;
}

static void handle_click(int mx, int my, gui_fp_t mxf, gui_fp_t myf, int sw, int sh) {
    if (handle_desktop_shell_click(mx, my, sw, sh)) return;

    /* close button on any window? */
    window_t* w = hit_window_z(mx, my);
    if (w) {
        int cx = w->x + w->w - CLOSE_W - 2;
        int cy = w->y + 2;
        if (point_in(mx, my, cx, cy, CLOSE_W - 2, TITLE_H - 4)) {
            (void)request_window_close(w, sw, sh);
            return;
        }
        if (point_in(mx, my, cx - TITLE_BUTTON_W, cy,
                     TITLE_BUTTON_W - 2, TITLE_H - 4)) {
            window_toggle_maximize(w, sw, sh);
            return;
        }
        if (point_in(mx, my, cx - TITLE_BUTTON_W * 2, cy,
                     TITLE_BUTTON_W - 2, TITLE_H - 4)) {
            window_minimize(w, sw, sh);
            return;
        }
        if (!w->maximized && point_in(mx, my,
                     w->x + w->w - RESIZE_GRIP,
                     w->y + w->h - RESIZE_GRIP,
                     RESIZE_GRIP,
                     RESIZE_GRIP)) {
            window_t* old_top = topmost();
            if (old_top && old_top != w) invalidate_window(sw, sh, old_top);
            invalidate_window(sw, sh, w);
            z_push_top(win_index(w));
            g_drag = DRAG_RESIZE;
            g_drag_idx = win_index(w);
            g_resize_start_mx = mx;
            g_resize_start_my = my;
            g_resize_start_w = w->w;
            g_resize_start_h = w->h;
            return;
        }
        /* title bar: raise + start drag */
        if (point_in(mx, my, w->x, w->y, w->w, TITLE_H)) {
            uint32_t now = sys_get_ticks();
            if (g_last_title_click_window == win_index(w) &&
                (uint32_t)(now - g_last_title_click_tick) <=
                    SMALLOS_TIMER_HZ / 3u) {
                g_last_title_click_window = -1;
                g_last_title_click_tick = 0;
                window_toggle_maximize(w, sw, sh);
                return;
            }
            g_last_title_click_window = win_index(w);
            g_last_title_click_tick = now;
            window_t* old_top = topmost();
            if (old_top && old_top != w) invalidate_window(sw, sh, old_top);
            invalidate_window(sw, sh, w);
            z_push_top(win_index(w));
            if (w->maximized) {
                w->x = w->restore_x;
                w->y = w->restore_y;
                w->w = w->restore_w;
                w->h = w->restore_h;
                w->maximized = 0;
                dispatch_window_resize(w, now);
            }
            g_drag = DRAG_MOVE;
            g_drag_idx = win_index(w);
            g_drag_dx_fp = mxf - fp_from_int(w->x);
            g_drag_dy_fp = myf - fp_from_int(w->y);
            g_drag_preview_x = w->x;
            g_drag_preview_y = w->y;
            g_drag_overlay_visible = 0;
            g_drag_overlay_x = w->x;
            g_drag_overlay_y = w->y;
            gui_request_frame_now(sys_get_ticks());
            return;
        }
        /* body click: raise */
        {
            window_t* old_top = topmost();
            if (old_top && old_top != w) invalidate_window(sw, sh, old_top);
            invalidate_window(sw, sh, w);
        }
        z_push_top(win_index(w));
        {
            gui_app_event_t event;
            memset(&event, 0, sizeof(event));
            event.type = GUI_APP_EVENT_POINTER_DOWN;
            event.x = mx - w->x;
            event.y = my - (w->y + TITLE_H);
            event.width = w->w;
            event.height = w->h - TITLE_H;
            event.buttons = SYS_MOUSE_BUTTON_LEFT;
            event.ticks = sys_get_ticks();
            apply_app_result(w, dispatch_app_event(w, &event), sw, sh);
            if (w->type == WT_CONFIG) invalidate_topbar(sw, sh);
        }
        return;
    }

    /* desktop icon? */
    int hi = icon_hit(mx, my);
    if (hi >= 0) {
        g_icons[hi].action(sw, sh);
        return;
    }
}

static void handle_wheel(int mx, int my, int wheel, int sw, int sh) {
    if (wheel == 0) return;

    window_t* w = hit_window_z(mx, my);
    if (!w) return;
    z_push_top(win_index(w));

    {
        gui_app_event_t event;
        memset(&event, 0, sizeof(event));
        event.type = GUI_APP_EVENT_WHEEL;
        event.x = mx - w->x;
        event.y = my - (w->y + TITLE_H);
        event.width = w->w;
        event.height = w->h - TITLE_H;
        event.wheel = wheel;
        event.ticks = sys_get_ticks();
        apply_app_result(w, dispatch_app_event(w, &event), sw, sh);
    }
}

int gui_main(int argc, char** argv) {
    gfx_context_t gfx;
    int rc;
    const char* initial_path = 0;

    for (int i = 1; i < argc; i++) {
        if (argv[i] && u_streq(argv[i], "--diagnostics"))
            g_diagnostics = 1;
        else if (argv[i] && argv[i][0] && !initial_path)
            initial_path = argv[i];
    }

    u_puts("gui: starting (ESC or q to exit)\n");
    rc = gfx_open(&gfx);
    if (rc == -1) { u_puts("gui: framebuffer not available\n"); return 0; }
    if (rc == -2) { u_puts("gui: display is already in use\n");  return 1; }
    if (rc == -3) { u_puts("gui: unsupported display size\n");   return 1; }
    if (rc == -4) { u_puts("gui: out of memory opening display\n"); return 1; }
    if (rc < 0)   { u_puts("gui: could not open display\n");     return 1; }

    int sw = (int)gfx.backbuffer.width;
    int sh = (int)gfx.backbuffer.height;
    g_screen_width = sw;
    g_screen_height = sh;
    gui_fp_t mxf = fp_from_int(sw / 2);
    gui_fp_t myf = fp_from_int(sh / 2);
    int mx = fp_to_int_round(mxf);
    int my = fp_to_int_round(myf);

    gui_window_stack_init(&g_window_stack);
    icons_layout(sw);
    {
        gui_native_ui_t native_ui = {
            draw_text, text_width, &GUI_WIDGET_THEME,
            COL_WIN_BG, COL_FRAME, COL_TEXT, COL_SUBTEXT,
            COL_HILIGHT, COL_HILIGHT_T
        };
        gui_native_apps_init(&native_ui);
    }
    init_app_registry();

    /* drain any stale mouse delta */
    { sys_mouse_state_t m; (void)sys_mouse_read(&m); }

    int prev_left = 0;
    int dirty = 1;
    int cursor_dirty = 1;
    int presented_mx = mx;
    int presented_my = my;
    int last_hover = hover_key(mx, my);
    gui_config_load();
    if (initial_path) action_editor(sw, sh, initial_path);
    invalidate_full(sw, sh);
    perf_init();
    g_startup_ram_bytes = sample_self_ram();
    g_clock_next_tick = sys_get_ticks() + SMALLOS_TIMER_HZ * 60u;

    while (!g_should_quit) {
        sys_input_event_t events[INPUT_BATCH];
        int got = 0;
        unsigned int input_flags = SYS_INPUT_FLAG_NONBLOCK;

        if (g_clock_next_tick && tick_due(sys_get_ticks(), g_clock_next_tick)) {
            invalidate_topbar(sw, sh);
            dirty = 1;
            g_clock_next_tick = sys_get_ticks() + SMALLOS_TIMER_HZ * 60u;
        }

        int n = read_input_coalesced(events, INPUT_BATCH, input_flags);
        if (perf_tick(sw, sh)) {
            dirty = 1;
        }
        if (n > 0) {
            got = 1;
            perf_note_input((unsigned int)n);

            for (int ei = 0; ei < n && !g_should_quit; ei++) {
                sys_input_event_t* ev = &events[ei];

                if (ev->type == SYS_INPUT_TYPE_KEY &&
                    (ev->flags & SYS_INPUT_KEY_PRESSED) != 0u) {
                    unsigned int a = ev->ascii & 0xFFu;
                    window_t* top = topmost();
                    unsigned int app_result = GUI_APP_RESULT_NONE;
                    if (g_shared_picker_active) {
                        gui_app_event_t picker_event;
                        memset(&picker_event, 0, sizeof(picker_event));
                        picker_event.type = GUI_APP_EVENT_KEY;
                        picker_event.key = ev->key;
                        picker_event.ascii = a;
                        picker_event.modifiers = ev->flags;
                        picker_event.ticks = ev->ticks;
                        (void)handle_shared_picker_event(&picker_event, sw, sh);
                        dirty = 1;
                        continue;
                    }
                    if ((ev->flags & SYS_INPUT_KEY_ALT) &&
                        ev->key == KEY_TAB) {
                        cycle_windows(sw, sh);
                        dirty = 1;
                        continue;
                    }
                    if ((ev->flags & SYS_INPUT_KEY_ALT) && top &&
                        ev->key == KEY_F9) {
                        window_minimize(top, sw, sh);
                        dirty = 1;
                        continue;
                    }
                    if ((ev->flags & SYS_INPUT_KEY_ALT) && top &&
                        ev->key == KEY_F10) {
                        window_toggle_maximize(top, sw, sh);
                        dirty = 1;
                        continue;
                    }
                    if ((ev->flags & SYS_INPUT_KEY_CTRL) &&
                        ev->key == KEY_ESC) {
                        g_start_open = !g_start_open;
                        g_start_selection = 0;
                        invalidate_full(sw, sh);
                        dirty = 1;
                        continue;
                    }
                    if (handle_start_key(ev->key, a, sw, sh)) {
                        dirty = 1;
                        continue;
                    }
                    if (top) {
                        gui_app_event_t app_event;
                        memset(&app_event, 0, sizeof(app_event));
                        app_event.type = GUI_APP_EVENT_KEY;
                        app_event.width = top->w;
                        app_event.height = top->h - TITLE_H;
                        app_event.key = ev->key;
                        app_event.ascii = a;
                        app_event.modifiers = ev->flags;
                        app_event.ticks = ev->ticks;
                        app_result = dispatch_app_event(top, &app_event);
                        apply_app_result(top, app_result, sw, sh);
                        if (g_dirty_count > 0) dirty = 1;
                    }
                    if (!(app_result & GUI_APP_RESULT_HANDLED)) {
                        if (a == 27u || ev->key == KEY_ESC ||
                            a == 'q' || a == 'Q') {
                            action_quit(sw, sh);
                        } else if ((a == 'x' || a == 'X') && top) {
                            (void)request_window_close(top, sw, sh);
                        } else if (a == 'f' || a == 'F') {
                            action_files(sw, sh);
                        } else if (a == 't' || a == 'T') {
                            action_shell(sw, sh);
                        } else if (a == 's' || a == 'S') {
                            action_system(sw, sh);
                        } else if (a == 'c' || a == 'C') {
                            action_config(sw, sh);
                        } else if (a == 'a' || a == 'A') {
                            action_about(sw, sh);
                        }
                        if (g_dirty_count > 0) dirty = 1;
                    }
                } else if (ev->type == SYS_INPUT_TYPE_MOUSE) {
                    int old_mx = mx;
                    int old_my = my;
                    int old_hover = last_hover;

                    if (ev->flags & SYS_INPUT_MOUSE_ABSOLUTE) {
                        mxf = abs_to_screen_fp(ev->abs_x, sw - 1);
                        myf = abs_to_screen_fp(ev->abs_y, sh - 1);
                    } else {
                        mxf = fp_clamp_int(mxf + fp_from_int(ev->dx), 0, sw - 1);
                        myf = fp_clamp_int(myf + fp_from_int(ev->dy), 0, sh - 1);
                    }
                    mx = fp_to_int_round(mxf);
                    my = fp_to_int_round(myf);
                    if (mx != old_mx || my != old_my) {
                        cursor_dirty = 1;
                    }
                    int left_now =
                        (ev->buttons & SYS_MOUSE_BUTTON_LEFT) != 0;
                    if (g_shared_picker_active) {
                        gui_app_event_t picker_event;
                        memset(&picker_event, 0, sizeof(picker_event));
                        picker_event.x = mx;
                        picker_event.y = my;
                        picker_event.buttons = ev->buttons;
                        picker_event.wheel = ev->wheel;
                        picker_event.ticks = ev->ticks;
                        if (ev->wheel)
                            picker_event.type = GUI_APP_EVENT_WHEEL;
                        else if (left_now && !prev_left)
                            picker_event.type = GUI_APP_EVENT_POINTER_DOWN;
                        else if (!left_now && prev_left)
                            picker_event.type = GUI_APP_EVENT_POINTER_UP;
                        else
                            picker_event.type = GUI_APP_EVENT_POINTER_MOVE;
                        (void)handle_shared_picker_event(&picker_event, sw, sh);
                        prev_left = left_now;
                        dirty = 1;
                        continue;
                    }
                    if (ev->wheel != 0) {
                        handle_wheel(mx, my, ev->wheel, sw, sh);
                        dirty = 1;
                    }
                    if (left_now && !prev_left) {
                        handle_click(mx, my, mxf, myf, sw, sh);
                        if (g_dirty_count > 0) dirty = 1;
                    } else if (!left_now && prev_left) {
                        if (g_drag == DRAG_MOVE && g_drag_idx >= 0) {
                            window_t* w = &g_wins[g_drag_idx];
                            if (w->active) {
                                if (g_drag_overlay_visible) {
                                    if (present_drag_preview(&gfx, drag_overlay_rect(), 0) < 0) {
                                        gfx_close(&gfx);
                                        u_puts("gui: present failed\n");
                                        return 1;
                                    }
                                    g_drag_overlay_visible = 0;
                                    cursor_dirty = 1;
                                }
                                invalidate_window(sw, sh, w);
                                w->x = g_drag_preview_x;
                                w->y = g_drag_preview_y;
                                if (my <= 2) {
                                    w->x = w->restore_x;
                                    w->y = w->restore_y;
                                    w->w = w->restore_w;
                                    w->h = w->restore_h;
                                    w->maximized = 0;
                                    window_toggle_maximize(w, sw, sh);
                                } else if (mx <= 2) {
                                    window_snap(w, sw, sh, -1);
                                } else if (mx >= sw - 3) {
                                    window_snap(w, sw, sh, 1);
                                }
                                invalidate_window(sw, sh, w);
                                dirty = 1;
                            }
                        }
                        g_drag = DRAG_NONE;
                        g_drag_idx = -1;
                        gui_cancel_frame();
                    }
                    if (g_drag == DRAG_MOVE && g_drag_idx >= 0) {
                        window_t* w = &g_wins[g_drag_idx];
                        if (w->active) {
                            gui_fp_t new_x_fp = fp_clamp_int(mxf - g_drag_dx_fp,
                                                             -w->w + 32,
                                                             sw - 32);
                            gui_fp_t new_y_fp = fp_clamp_int(myf - g_drag_dy_fp,
                                                             0,
                                                             sh - TASKBAR_H - TITLE_H);
                            int new_x = fp_to_int_round(new_x_fp);
                            int new_y = fp_to_int_round(new_y_fp);
                            if (new_x != g_drag_preview_x || new_y != g_drag_preview_y) {
                                g_drag_preview_x = new_x;
                                g_drag_preview_y = new_y;
                                cursor_dirty = 1;
                                gui_request_frame(sys_get_ticks());
                            }
                        }
                    }
                    if (g_drag == DRAG_RESIZE && g_drag_idx >= 0) {
                        window_t* w = &g_wins[g_drag_idx];
                        if (w->active) {
                            const window_app_ops_t* ops = window_app_ops(w->type);
                            int max_w = sw - w->x;
                            int max_h = sh - TASKBAR_H - w->y;
                            int min_w = ops ? ops->min_width : WINDOW_MIN_W;
                            int min_h = ops ? ops->min_height : WINDOW_MIN_H;
                            int new_w;
                            int new_h;
                            if (max_w < min_w) max_w = min_w;
                            if (max_h < min_h) max_h = min_h;
                            new_w = clampi(g_resize_start_w + mx - g_resize_start_mx,
                                         min_w, max_w);
                            new_h = clampi(g_resize_start_h + my - g_resize_start_my,
                                         min_h, max_h);
                            if (new_w != w->w || new_h != w->h) {
                                gui_app_event_t resize_event;
                                invalidate_window(sw, sh, w);
                                w->w = new_w;
                                w->h = new_h;
                                memset(&resize_event, 0, sizeof(resize_event));
                                resize_event.type = GUI_APP_EVENT_RESIZE;
                                resize_event.width = w->w;
                                resize_event.height = w->h - TITLE_H;
                                resize_event.ticks = ev->ticks;
                                (void)dispatch_app_event(w, &resize_event);
                                invalidate_window(sw, sh, w);
                                dirty = 1;
                            }
                        }
                    }
                    if (!left_now && prev_left) {
                        window_t* target = 0;
                        for (int i = 0; i < MAX_WINDOWS; i++) {
                            if (g_wins[i].active && g_wins[i].pressed_widget) {
                                target = &g_wins[i];
                                break;
                            }
                        }
                        if (!target) target = hit_window_z(mx, my);
                        if (target) {
                            gui_app_event_t app_event;
                            memset(&app_event, 0, sizeof(app_event));
                            app_event.type = GUI_APP_EVENT_POINTER_UP;
                            app_event.x = mx - target->x;
                            app_event.y = my - (target->y + TITLE_H);
                            app_event.width = target->w;
                            app_event.height = target->h - TITLE_H;
                            app_event.ticks = ev->ticks;
                            apply_app_result(target,
                                dispatch_app_event(target, &app_event), sw, sh);
                            if (g_dirty_count > 0) dirty = 1;
                        }
                    } else if ((mx != old_mx || my != old_my) &&
                               g_drag == DRAG_NONE) {
                        window_t* target = 0;
                        for (int i = 0; i < MAX_WINDOWS; i++) {
                            if (g_wins[i].active && g_wins[i].pressed_widget) {
                                target = &g_wins[i];
                                break;
                            }
                        }
                        if (!target) target = hit_window_z(mx, my);
                        if (target) {
                            gui_app_event_t app_event;
                            memset(&app_event, 0, sizeof(app_event));
                            app_event.type = GUI_APP_EVENT_POINTER_MOVE;
                            app_event.x = mx - target->x;
                            app_event.y = my - (target->y + TITLE_H);
                            app_event.width = target->w;
                            app_event.height = target->h - TITLE_H;
                            app_event.buttons = ev->buttons;
                            app_event.ticks = ev->ticks;
                            apply_app_result(target,
                                dispatch_app_event(target, &app_event), sw, sh);
                            if (g_dirty_count > 0) dirty = 1;
                        }
                    }
                    prev_left = left_now;
                    last_hover = hover_key(mx, my);
                    if (last_hover != old_hover) {
                        int hover_dirty = invalidate_hover_key(sw, sh, old_hover);
                        hover_dirty |= invalidate_hover_key(sw, sh, last_hover);
                        if (hover_dirty) dirty = 1;
                    }
                }
            }
        }

        if (poll_shell_windows(sw, sh)) dirty = 1;
        sync_window_focus(sw, sh);
        if (dispatch_due_app_ticks(sw, sh, sys_get_ticks())) dirty = 1;

        if (g_launch_kind != LAUNCH_NONE) {
            int launch_rc = run_queued_launch(&gfx, &sw, &sh);
            if (launch_rc < 0) return 1;
            mxf = fp_clamp_int(mxf, 0, sw - 1);
            myf = fp_clamp_int(myf, 0, sh - 1);
            mx = fp_to_int_round(mxf);
            my = fp_to_int_round(myf);
            presented_mx = mx;
            presented_my = my;
            last_hover = hover_key(mx, my);
            cursor_dirty = 1;
            dirty = 1;
            invalidate_full(sw, sh);
        }

        /* A logical event without pixel damage must not become a full repaint. */
        if (dirty && g_dirty_count == 0) {
            dirty = 0;
        }

        if (dirty) {
            if (g_drag_overlay_visible) {
                if (present_drag_preview(&gfx, drag_overlay_rect(), 0) < 0) {
                    gfx_close(&gfx);
                    u_puts("gui: present failed\n");
                    return 1;
                }
                g_drag_overlay_visible = 0;
                cursor_dirty = 1;
            }
            if (present_dirty_scene(&gfx, mx, my,
                                    presented_mx, presented_my) < 0) {
                gfx_close(&gfx);
                u_puts("gui: present failed\n");
                return 1;
            }
            dirty = 0;
            if (cursor_dirty) {
                if (present_cursor_move(&gfx, presented_mx, presented_my, mx, my) < 0) {
                    gfx_close(&gfx);
                    u_puts("gui: present failed\n");
                    return 1;
                }
                cursor_dirty = 0;
            }
            if (g_drag == DRAG_MOVE && g_drag_idx >= 0) {
                uint32_t now = sys_get_ticks();
                gui_request_frame_now(now);
                if (present_gui_frame(&gfx, mx, my, now) < 0) {
                    gfx_close(&gfx);
                    u_puts("gui: present failed\n");
                    return 1;
                }
            }
            last_hover = hover_key(mx, my);
            presented_mx = mx;
            presented_my = my;
        } else if (cursor_dirty) {
            if (g_drag == DRAG_MOVE && g_drag_idx >= 0) {
                if (present_cursor_move_with_drag_overlay(&gfx,
                                                          presented_mx,
                                                          presented_my,
                                                          mx,
                                                          my) < 0) {
                    gfx_close(&gfx);
                    u_puts("gui: present failed\n");
                    return 1;
                }
            } else if (present_cursor_move(&gfx, presented_mx, presented_my, mx, my) < 0) {
                gfx_close(&gfx);
                u_puts("gui: present failed\n");
                return 1;
            }
            cursor_dirty = 0;
            if (gui_frame_due(sys_get_ticks())) {
                if (present_gui_frame(&gfx, mx, my, sys_get_ticks()) < 0) {
                    gfx_close(&gfx);
                    u_puts("gui: present failed\n");
                    return 1;
                }
            }
            presented_mx = mx;
            presented_my = my;
        } else if (gui_frame_due(sys_get_ticks())) {
            if (present_gui_frame(&gfx, mx, my, sys_get_ticks()) < 0) {
                gfx_close(&gfx);
                u_puts("gui: present failed\n");
                return 1;
            }
        }

        if (!got) {
            uint32_t now = sys_get_ticks();
            uint32_t deadline = gui_wait_deadline(now);
            struct pollfd fds[MAX_WINDOWS];
            unsigned int fd_count = shell_poll_fds(fds, MAX_WINDOWS);
            if (deadline == 0u || !tick_due(now, deadline)) {
                int wait_result = smallos_input_fd_wait_until(fds, fd_count,
                                                               deadline);
                if (wait_result & SYS_INPUT_FD_WAIT_READY) {
                    g_perf.pty_wakeups++;
                    g_perf.total_pty_wakeups++;
                }
                perf_resume_from_idle();
            } else {
                sys_yield();
            }
        }
    }

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_wins[i].active) close_window(&g_wins[i]);
    }
    gfx_close(&gfx);
    if (g_diagnostics) diagnostics_print();
    u_puts("gui: exiting\n");
    return 0;
}
