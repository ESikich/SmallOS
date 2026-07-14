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
#include "cursor.h"
#include "damage.h"
#include "region.h"
#include "shell_window.h"
#include "widgets.h"
#include "window.h"
#include "../editor_model.h"

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
#define COL_TOPBAR    0x00FFFFFFu
#define COL_SHADOW    0x00606060u

static const gui_widget_theme_t GUI_WIDGET_THEME = {
    COL_BTN_BG, 0x00D8D8D8u, 0x00A8A8A8u, COL_FRAME,
    COL_TEXT, 0x00808080u, COL_TITLE_BG
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

/* ---------------- window model ---------------- */

#define MAX_WINDOWS GUI_WINDOW_CAPACITY
#define MAX_ROWS    256
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
#define GUI_SHELL_POLL_FPS 60u
#define GUI_SHELL_POLL_TICKS \
    ((SMALLOS_TIMER_HZ + GUI_SHELL_POLL_FPS - 1u) / GUI_SHELL_POLL_FPS)
#define GUI_CONFIG_PATH "/etc/gui.conf"
#define GUI_CONFIG_MAX 128u

typedef enum {
    WT_FILES = 1,
    WT_SYSTEM,
    WT_CONFIG,
    WT_ABOUT,
    WT_SHELL,
    WT_EDITOR,
} win_type_t;

typedef struct {
    int scroll;
    int scroll_drag_offset;
    char cwd[256];
    char rows[MAX_ROWS][NAME_MAX + 1];
    int row_dir[MAX_ROWS];
    int row_count;
    char status[80];
} files_window_state_t;

typedef struct {
    editor_model_t model;
    uint32_t row;
    uint32_t column;
    uint32_t top;
    uint32_t hscroll;
    uint32_t next_blink;
    int caret_visible;
    int confirm_close;
    int confirm_choice;
    uint32_t anchor_row;
    uint32_t anchor_column;
    int selection_active;
    int scroll_drag_offset;
    char status[80];
} editor_window_state_t;

typedef struct {
    int   active;
    win_type_t type;
    int   x, y, w, h;
    void* state;
    int focused_widget;
    int pressed_widget;
    uint32_t next_tick;
} window_t;

typedef struct {
    const char* title;
    unsigned int state_size;
    int default_width;
    int default_height;
    int min_width;
    int min_height;
    uint32_t tick_interval;
    void (*open)(window_t* window, const char* argument);
    void (*close)(window_t* window);
    void (*draw)(gfx_surface_t* surface, window_t* window, int mx, int my);
    unsigned int (*event)(window_t* window, const gui_app_event_t* event);
} window_app_ops_t;

static const window_app_ops_t* window_app_ops(win_type_t type);
static void action_editor(int sw, int sh, const char* path);

#define FILES_STATE(w) ((files_window_state_t*)(w)->state)
#define SHELL_STATE(w) ((gui_shell_window_t*)(w)->state)
#define EDITOR_STATE(w) ((editor_window_state_t*)(w)->state)

static window_t g_wins[MAX_WINDOWS];
static int g_screen_width;
static int g_screen_height;
static int g_last_file_win = -1;
static int g_last_file_row = -1;
static uint32_t g_last_file_tick;
#define GUI_CLIPBOARD_CAPACITY 8192u
static char g_editor_clipboard[GUI_CLIPBOARD_CAPACITY];

static int point_in(int x, int y, int rx, int ry, int rw, int rh);

#define TITLE_H 14
#define CLOSE_W 14
#define ROW_H   12
#define RESIZE_GRIP 10
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

static void icons_layout(int sw);

typedef struct {
    uint32_t window_start;
    uint32_t last_loop;
    unsigned int loops;
    unsigned int presents;
    unsigned int input_events;
    unsigned int max_loop_gap;
    unsigned int max_present_ticks;
    unsigned int shown_loops;
    unsigned int shown_presents;
    unsigned int shown_input_events;
    unsigned int shown_max_loop_gap;
    unsigned int shown_max_present_ticks;
} gui_perf_t;

static gui_perf_t g_perf;
static int g_perf_visible = 0;

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
    char buf[48];
    int fd;

    u_strcpy_n(buf, "perf_visible=", sizeof(buf));
    u_strcat_n(buf, g_perf_visible ? "1\n" : "0\n", sizeof(buf));

    fd = sys_open_mode(GUI_CONFIG_PATH,
                       SYS_OPEN_MODE_WRITE |
                       SYS_OPEN_MODE_CREATE |
                       SYS_OPEN_MODE_TRUNC);
    if (fd < 0) return;
    (void)sys_writefd(fd, buf, u_strlen(buf));
    (void)sys_fsync(fd);
    (void)sys_close(fd);
}

static gui_window_stack_t g_window_stack;

static int win_index(window_t* w) { return (int)(w - g_wins); }

static void z_remove_idx(int win_index_val) {
    gui_window_stack_remove(&g_window_stack, win_index_val);
}

static void z_push_top(int win_index_val) {
    gui_window_stack_raise(&g_window_stack, win_index_val);
}

static window_t* topmost(void) {
    int index = gui_window_stack_top(&g_window_stack);
    return index < 0 ? 0 : &g_wins[index];
}

static window_t* hit_window_z(int mx, int my) {
    for (int i = gui_window_stack_count(&g_window_stack) - 1; i >= 0; i--) {
        window_t* w = &g_wins[gui_window_stack_at(&g_window_stack, i)];
        if (!w->active) continue;
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
    return 0;
}

static void close_window(window_t* w) {
    const window_app_ops_t* ops;
    if (!w) return;
    ops = window_app_ops(w->type);
    if (ops && ops->close) ops->close(w);
    if (w->state) free(w->state);
    w->state = 0;
    z_remove_idx(win_index(w));
    w->active = 0;
}

static gui_rect_t window_screen_rect(window_t* w) {
    if (!w || !w->active) return make_rect(0, 0, 0, 0);
    return make_rect(w->x, w->y, w->w + 3, w->h + 3);
}

static void dirty_clear(void) {
    gui_damage_clear(&g_damage);
}

static void invalidate_full(int sw, int sh) {
    gui_damage_full(&g_damage, sw, sh);
}

static void invalidate_rect(int sw, int sh, gui_rect_t r) {
    gui_damage_add(&g_damage, r, sw, sh);
}

static void invalidate_window(int sw, int sh, window_t* w) {
    invalidate_rect(sw, sh, window_screen_rect(w));
}

static void invalidate_topbar(int sw, int sh) {
    (void)sh;
    invalidate_rect(sw, sh, make_rect(0, 0, sw, 15));
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
}

static int perf_tick(int sw, int sh) {
    uint32_t now = sys_get_ticks();
    unsigned int gap = now - g_perf.last_loop;

    g_perf.last_loop = now;
    g_perf.loops++;
    if (gap > g_perf.max_loop_gap) {
        g_perf.max_loop_gap = gap;
    }

    if ((uint32_t)(now - g_perf.window_start) >= GUI_PERF_WINDOW_TICKS) {
        g_perf.shown_loops = g_perf.loops;
        g_perf.shown_presents = g_perf.presents;
        g_perf.shown_input_events = g_perf.input_events;
        g_perf.shown_max_loop_gap = g_perf.max_loop_gap;
        g_perf.shown_max_present_ticks = g_perf.max_present_ticks;
        g_perf.window_start = now;
        g_perf.loops = 0;
        g_perf.presents = 0;
        g_perf.input_events = 0;
        g_perf.max_loop_gap = 0;
        g_perf.max_present_ticks = 0;
        invalidate_topbar(sw, sh);
        return 1;
    }

    return 0;
}

static void perf_note_input(unsigned int events) {
    g_perf.input_events += events;
}

static void perf_note_present(uint32_t start_tick) {
    uint32_t now = sys_get_ticks();
    unsigned int ticks = now - start_tick;

    g_perf.presents++;
    if (ticks > g_perf.max_present_ticks) {
        g_perf.max_present_ticks = ticks;
    }
}

static void perf_resume_from_idle(void) {
    uint32_t now = sys_get_ticks();

    g_perf.window_start = now;
    g_perf.last_loop = now;
    g_perf.loops = 0;
    g_perf.presents = 0;
    g_perf.input_events = 0;
    g_perf.max_loop_gap = 0;
    g_perf.max_present_ticks = 0;
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

/* ---------------- path utilities ---------------- */

static void path_normalize_parent(char* path) {
    /* drop trailing slash if not root */
    unsigned int n = u_strlen(path);
    if (n > 1 && path[n - 1] == '/') { path[n - 1] = 0; n--; }
    /* find last '/' */
    int last = -1;
    for (unsigned int i = 0; i < n; i++) if (path[i] == '/') last = (int)i;
    if (last <= 0) { u_strcpy_n(path, "/", 256); return; }
    path[last] = 0;
}

static void path_append(char* path, const char* name, unsigned int cap) {
    unsigned int n = u_strlen(path);
    if (n == 0 || path[n - 1] != '/') u_strcat_n(path, "/", cap);
    u_strcat_n(path, name, cap);
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

static void queue_launch(window_t* w, const char* path) {
    files_window_state_t* files = FILES_STATE(w);
    launch_kind_t kind = launch_kind_for_path(path);
    if (kind == LAUNCH_NONE) {
        u_strcpy_n(files->status, "No launcher for this file type", sizeof(files->status));
        return;
    }
    g_launch_kind = kind;
    u_strcpy_n(g_launch_path, path, sizeof(g_launch_path));
    u_strcpy_n(files->status, "Launching...", sizeof(files->status));
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

/* ---------------- file load ---------------- */

static void load_dir(window_t* w) {
    files_window_state_t* files = FILES_STATE(w);
    files->row_count = 0;
    files->scroll = 0;

    /* synthesize ".." for non-root */
    if (!u_streq(files->cwd, "/")) {
        u_strcpy_n(files->rows[files->row_count], "..", NAME_MAX + 1);
        files->row_dir[files->row_count] = 1;
        files->row_count++;
    }

    DIR* d = opendir(files->cwd);
    if (!d) {
        u_strcpy_n(files->rows[files->row_count], "<cannot open>", NAME_MAX + 1);
        files->row_dir[files->row_count] = 0;
        files->row_count++;
        return;
    }

    struct dirent* e;
    while ((e = readdir(d)) != 0 && files->row_count < MAX_ROWS) {
        u_strcpy_n(files->rows[files->row_count], e->d_name, NAME_MAX + 1);
        files->row_dir[files->row_count] = e->d_is_dir;
        files->row_count++;
    }
    closedir(d);
}

/* ---------------- drawing windows ---------------- */

static void draw_title_bar(gfx_surface_t* s, window_t* w, int focused, const char* title) {
    unsigned int bg = focused ? COL_TITLE_BG : COL_TITLE_IDLE_BG;
    fillr(s, w->x, w->y, w->w, TITLE_H, bg);
    draw_text(s, w->x + 4, w->y + 4, title, COL_TITLE_FG);
    /* close button */
    int cx = w->x + w->w - CLOSE_W - 2;
    int cy = w->y + 2;
    fillr(s, cx, cy, CLOSE_W - 2, TITLE_H - 4, COL_WIN_BG);
    rect(s, cx, cy, CLOSE_W - 2, TITLE_H - 4, COL_FRAME);
    /* draw 'x' inside */
    int ix = cx + 2, iy = cy + 2;
    for (int i = 0; i < 6; i++) {
        gui_put_pixel(s, ix + i, iy + i, COL_FRAME);
        gui_put_pixel(s, ix + 5 - i, iy + i, COL_FRAME);
    }
}

static void draw_files_body(gfx_surface_t* s, window_t* w, int mx, int my) {
    files_window_state_t* files = FILES_STATE(w);
    int bx = w->x;
    int by = w->y + TITLE_H;
    int bw = w->w;
    int bh = w->h - TITLE_H;
    fillr(s, bx, by, bw, bh, COL_WIN_BG);

    /* breadcrumb */
    draw_text(s, bx + 4, by + 2, "Path:", COL_SUBTEXT);
    draw_text(s, bx + 36, by + 2, files->cwd, COL_TEXT);
    hline(s, bx, by + 12, bw, COL_FRAME);

    int row_top = by + 14;
    int status_h = 13;
    int row_area = bh - 14 - status_h;
    int visible = row_area / ROW_H;
    if (visible < 1) visible = 1;

    for (int i = 0; i < visible && (i + files->scroll) < files->row_count; i++) {
        int idx = i + files->scroll;
        int ry = row_top + i * ROW_H;
        int hover = (mx >= bx && mx < bx + bw - 12 && my >= ry && my < ry + ROW_H);
        if (hover) fillr(s, bx, ry, bw - 12, ROW_H, COL_HILIGHT);
        unsigned int color = hover ? COL_HILIGHT_T : COL_TEXT;
        /* tiny icon: [D] for dir, [F] for file */
        draw_text(s, bx + 4, ry + 2, files->row_dir[idx] ? "[D]" : "[F]", color);
        draw_text(s, bx + 28, ry + 2, files->rows[idx], color);
    }

    gui_widget_scrollbar(s, make_rect(bx + bw - 10, by + 14, 10, row_area),
                         files->row_count, visible, files->scroll,
                         (gui_widget_state_t){0,
                             w->pressed_widget == 1, 0, 0},
                         &GUI_WIDGET_THEME);

    hline(s, bx, by + bh - status_h, bw, COL_FRAME);
    if (files->status[0]) {
        draw_text(s, bx + 4, by + bh - status_h + 4, files->status, COL_SUBTEXT);
    }

    /* outer frame */
    rect(s, w->x, w->y, w->w, w->h, COL_FRAME);
}

static void draw_system_body(gfx_surface_t* s, window_t* w) {
    int bx = w->x;
    int by = w->y + TITLE_H;
    int bw = w->w;
    int bh = w->h - TITLE_H;
    fillr(s, bx, by, bw, bh, COL_WIN_BG);

    sys_fsinfo_t fs;
    sys_display_info_t di;
    int has_fs = sys_fsinfo(&fs) == 0;
    int has_di = sys_display_info(&di) == 0;
    uint32_t ticks = sys_get_ticks();
    int pid = sys_getpid();

    int ty = by + 8;
    char buf[64];
    char num[16];

    draw_text(s, bx + 8, ty, "Display", COL_SUBTEXT); ty += 12;
    if (has_di) {
        u_strcpy_n(buf, "  ", sizeof(buf));
        utoa10(di.width, num);  u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, " x ", sizeof(buf));
        utoa10(di.height, num); u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, " @ ", sizeof(buf));
        utoa10(di.bpp, num);    u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, " bpp", sizeof(buf));
        draw_text(s, bx + 8, ty, buf, COL_TEXT); ty += 12;
        u_strcpy_n(buf, "  pitch=", sizeof(buf));
        utoa10(di.pitch, num);  u_strcat_n(buf, num, sizeof(buf));
        draw_text(s, bx + 8, ty, buf, COL_TEXT); ty += 14;
    } else {
        draw_text(s, bx + 8, ty, "  (unavailable)", COL_TEXT); ty += 14;
    }

    draw_text(s, bx + 8, ty, "Filesystem", COL_SUBTEXT); ty += 12;
    if (has_fs) {
        u_strcpy_n(buf, "  total: ", sizeof(buf));
        utoa10(fs.total_bytes / 1024u, num); u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, " KB", sizeof(buf));
        draw_text(s, bx + 8, ty, buf, COL_TEXT); ty += 12;
        u_strcpy_n(buf, "  used:  ", sizeof(buf));
        utoa10(fs.used_bytes / 1024u, num); u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, " KB", sizeof(buf));
        draw_text(s, bx + 8, ty, buf, COL_TEXT); ty += 12;
        u_strcpy_n(buf, "  free:  ", sizeof(buf));
        utoa10(fs.free_bytes / 1024u, num); u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, " KB", sizeof(buf));
        draw_text(s, bx + 8, ty, buf, COL_TEXT); ty += 12;
        u_strcpy_n(buf, "  blocks: ", sizeof(buf));
        utoa10(fs.free_clusters, num); u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, " / ", sizeof(buf));
        utoa10(fs.total_clusters, num); u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, " free", sizeof(buf));
        draw_text(s, bx + 8, ty, buf, COL_TEXT); ty += 14;
    } else {
        draw_text(s, bx + 8, ty, "  (unavailable)", COL_TEXT); ty += 14;
    }

    draw_text(s, bx + 8, ty, "Process", COL_SUBTEXT); ty += 12;
    u_strcpy_n(buf, "  pid: ", sizeof(buf));
    utoa10((unsigned int)pid, num); u_strcat_n(buf, num, sizeof(buf));
    draw_text(s, bx + 8, ty, buf, COL_TEXT); ty += 12;
    u_strcpy_n(buf, "  ticks: ", sizeof(buf));
    utoa10(ticks, num); u_strcat_n(buf, num, sizeof(buf));
    draw_text(s, bx + 8, ty, buf, COL_TEXT); ty += 12;

    rect(s, w->x, w->y, w->w, w->h, COL_FRAME);
}

static void draw_config_body(gfx_surface_t* s, window_t* w) {
    int bx = w->x;
    int by = w->y + TITLE_H;
    int bw = w->w;
    int bh = w->h - TITLE_H;
    int row_y = by + 30;

    fillr(s, bx, by, bw, bh, COL_WIN_BG);
    draw_text(s, bx + 12, by + 12, "GUI", COL_SUBTEXT);

    gui_widget_checkbox(s, make_rect(bx + 8, row_y - 4, bw - 16, 20),
                        "Perf readout", g_perf_visible,
                        (gui_widget_state_t){0, 0, 1, 0},
                        &GUI_WIDGET_THEME, draw_text);

    rect(s, w->x, w->y, w->w, w->h, COL_FRAME);
}

static void draw_about_body(gfx_surface_t* s, window_t* w) {
    int bx = w->x;
    int by = w->y + TITLE_H;
    fillr(s, bx, by, w->w, w->h - TITLE_H, COL_WIN_BG);
    draw_text(s, bx + 12, by + 12, "SmallOS GUI", COL_TEXT);
    draw_text(s, bx + 12, by + 28, "Click an icon on the desktop", COL_SUBTEXT);
    draw_text(s, bx + 12, by + 40, "to open a window.", COL_SUBTEXT);
    draw_text(s, bx + 12, by + 60, "Drag the title bar to move.", COL_SUBTEXT);
    draw_text(s, bx + 12, by + 72, "Click X to close.", COL_SUBTEXT);
    draw_text(s, bx + 12, by + 92, "Press ESC or Q to exit gui.", COL_SUBTEXT);
    rect(s, w->x, w->y, w->w, w->h, COL_FRAME);
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

static void files_app_open(window_t* w, const char* argument) {
    files_window_state_t* files = FILES_STATE(w);
    (void)argument;
    u_strcpy_n(files->cwd, "/", sizeof(files->cwd));
    u_strcpy_n(files->status, "Double-click files to open", sizeof(files->status));
    load_dir(w);
}

static void shell_app_open(window_t* w, const char* argument) {
    (void)argument;
    gui_shell_open(SHELL_STATE(w));
}

static void shell_app_close(window_t* w) {
    if (w->state) gui_shell_close(SHELL_STATE(w));
}

static void files_app_draw(gfx_surface_t* s, window_t* w, int mx, int my) {
    draw_files_body(s, w, mx, my);
}

static void system_app_draw(gfx_surface_t* s, window_t* w, int mx, int my) {
    (void)mx;
    (void)my;
    draw_system_body(s, w);
}

static void config_app_draw(gfx_surface_t* s, window_t* w, int mx, int my) {
    (void)mx;
    (void)my;
    draw_config_body(s, w);
}

static void about_app_draw(gfx_surface_t* s, window_t* w, int mx, int my) {
    (void)mx;
    (void)my;
    draw_about_body(s, w);
}

static void shell_app_draw(gfx_surface_t* s, window_t* w, int mx, int my) {
    (void)mx;
    (void)my;
    draw_shell_body(s, w);
}

static unsigned int shell_app_event(window_t* w,
                                    const gui_app_event_t* event) {
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

static unsigned int files_app_event(window_t* w,
                                    const gui_app_event_t* event) {
    files_window_state_t* files = FILES_STATE(w);
    int row_area = (w->h - TITLE_H) - 27;
    int visible = row_area / ROW_H;
    int max_scroll;
    gui_rect_t track;
    gui_rect_t thumb;
    if (visible < 1) visible = 1;
    track = make_rect(w->w - 10, 14, 10, row_area);
    thumb = gui_widget_scroll_thumb(track, files->row_count, visible,
                                    files->scroll);
    max_scroll = files->row_count - visible;
    if (max_scroll < 0) max_scroll = 0;
    if (event->type == GUI_APP_EVENT_WHEEL) {
        files->scroll -= event->wheel * 3;
        if (files->scroll < 0) files->scroll = 0;
        if (files->scroll > max_scroll) files->scroll = max_scroll;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_MOVE &&
        w->pressed_widget == 1) {
        files->scroll = gui_widget_scroll_offset(
            track, files->row_count, visible, event->y,
            files->scroll_drag_offset);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP &&
        w->pressed_widget == 1) {
        w->pressed_widget = 0;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type != GUI_APP_EVENT_POINTER_DOWN)
        return GUI_APP_RESULT_NONE;
    if (gui_widget_hit(track, event->x, event->y)) {
        if (gui_widget_hit(thumb, event->x, event->y)) {
            w->pressed_widget = 1;
            files->scroll_drag_offset = event->y - thumb.y;
        } else if (event->y < thumb.y)
            files->scroll -= visible;
        else
            files->scroll += visible;
        if (files->scroll < 0) files->scroll = 0;
        if (files->scroll > max_scroll) files->scroll = max_scroll;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->y >= 14 && event->y < 14 + visible * ROW_H) {
        int row = (event->y - 14) / ROW_H + files->scroll;
        if (row >= 0 && row < files->row_count) {
            if (files->row_dir[row]) {
                if (u_streq(files->rows[row], ".."))
                    path_normalize_parent(files->cwd);
                else
                    path_append(files->cwd, files->rows[row],
                                sizeof(files->cwd));
                load_dir(w);
            } else {
                uint32_t now = event->ticks;
                int widx = win_index(w);
                int is_double = g_last_file_win == widx &&
                    g_last_file_row == row &&
                    (uint32_t)(now - g_last_file_tick) < 35u;
                char target[256];
                u_strcpy_n(target, files->cwd, sizeof(target));
                path_append(target, files->rows[row], sizeof(target));
                if (is_double) {
                    launch_kind_t kind = launch_kind_for_path(target);
                    if (kind == LAUNCH_EDIT) {
                        action_editor(g_screen_width, g_screen_height, target);
                        u_strcpy_n(files->status, "Opened in Editor",
                                   sizeof(files->status));
                    } else {
                        queue_launch(w, target);
                    }
                    g_last_file_win = -1;
                    g_last_file_row = -1;
                    g_last_file_tick = 0;
                } else {
                    u_strcpy_n(files->status, "Double-click to open ",
                               sizeof(files->status));
                    u_strcat_n(files->status, files->rows[row],
                               sizeof(files->status));
                    g_last_file_win = widx;
                    g_last_file_row = row;
                    g_last_file_tick = now;
                }
            }
        }
    }
    return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
}

static unsigned int config_app_event(window_t* w,
                                     const gui_app_event_t* event) {
    if (event->type == GUI_APP_EVENT_POINTER_DOWN &&
        point_in(event->x, event->y, 8, 26, w->w - 16, 20)) {
        g_perf_visible = !g_perf_visible;
        gui_config_save();
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_KEY &&
        (event->key == KEY_ENTER || event->key == KEY_SPACE)) {
        g_perf_visible = !g_perf_visible;
        gui_config_save();
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    return GUI_APP_RESULT_NONE;
}

#define EDITOR_TOOLBAR_H 16
#define EDITOR_STATUS_H 14
#define EDITOR_LINE_H 8
#define EDITOR_WIDGET_SAVE 1
#define EDITOR_WIDGET_DISCARD 2
#define EDITOR_WIDGET_CANCEL 3
#define EDITOR_WIDGET_SCROLLBAR 10
#define EDITOR_WIDGET_TEXT 11

static void editor_set_status(editor_window_state_t* editor,
                              const char* status) {
    u_strcpy_n(editor->status, status, sizeof(editor->status));
}

static int editor_position_before(uint32_t ar, uint32_t ac,
                                  uint32_t br, uint32_t bc) {
    return ar < br || (ar == br && ac < bc);
}

static int editor_has_selection(const editor_window_state_t* editor) {
    return editor->selection_active &&
        (editor->anchor_row != editor->row ||
         editor->anchor_column != editor->column);
}

static void editor_selection_bounds(const editor_window_state_t* editor,
                                    uint32_t* first_row,
                                    uint32_t* first_column,
                                    uint32_t* last_row,
                                    uint32_t* last_column) {
    if (editor_position_before(editor->anchor_row, editor->anchor_column,
                               editor->row, editor->column)) {
        *first_row = editor->anchor_row;
        *first_column = editor->anchor_column;
        *last_row = editor->row;
        *last_column = editor->column;
    } else {
        *first_row = editor->row;
        *first_column = editor->column;
        *last_row = editor->anchor_row;
        *last_column = editor->anchor_column;
    }
}

static void editor_clear_selection(editor_window_state_t* editor) {
    editor->selection_active = 0;
    editor->anchor_row = editor->row;
    editor->anchor_column = editor->column;
}

static int editor_delete_selection(editor_window_state_t* editor) {
    uint32_t first_row, first_column, last_row, last_column;
    if (!editor_has_selection(editor)) return 0;
    editor_selection_bounds(editor, &first_row, &first_column,
                            &last_row, &last_column);
    if (!editor_model_delete_range(&editor->model, first_row, first_column,
                                   last_row, last_column)) return 0;
    editor->row = first_row;
    editor->column = first_column;
    editor_clear_selection(editor);
    return 1;
}

static int editor_copy_selection(editor_window_state_t* editor) {
    uint32_t first_row, first_column, last_row, last_column;
    unsigned int used = 0;
    if (!editor_has_selection(editor)) return 0;
    editor_selection_bounds(editor, &first_row, &first_column,
                            &last_row, &last_column);
    for (uint32_t row = first_row; row <= last_row; row++) {
        uint32_t start = row == first_row ? first_column : 0u;
        uint32_t end = row == last_row ? last_column :
            editor_model_line_length(&editor->model, row);
        const char* line = editor->model.lines[row];
        for (uint32_t column = start;
             column < end && used + 1u < GUI_CLIPBOARD_CAPACITY; column++)
            g_editor_clipboard[used++] = line[column];
        if (row != last_row && used + 1u < GUI_CLIPBOARD_CAPACITY)
            g_editor_clipboard[used++] = '\n';
    }
    g_editor_clipboard[used] = '\0';
    return 1;
}

static int editor_paste(editor_window_state_t* editor) {
    int changed = 0;
    if (!g_editor_clipboard[0]) return 0;
    (void)editor_delete_selection(editor);
    for (unsigned int i = 0; g_editor_clipboard[i]; i++) {
        if (g_editor_clipboard[i] == '\n') {
            if (!editor_model_split_line(&editor->model, editor->row,
                                         editor->column)) break;
            editor->row++;
            editor->column = 0;
            changed = 1;
        } else {
            if (!editor_model_insert_char(&editor->model, editor->row,
                                          editor->column,
                                          g_editor_clipboard[i])) break;
            editor->column++;
            changed = 1;
        }
    }
    editor_clear_selection(editor);
    return changed;
}

static int editor_visible_rows(const window_t* w) {
    int rows = (w->h - TITLE_H - EDITOR_TOOLBAR_H - EDITOR_STATUS_H - 4) /
               EDITOR_LINE_H;
    return rows < 1 ? 1 : rows;
}

static int editor_visible_cols(const window_t* w) {
    int cols = (w->w - 18) / 6;
    return cols < 1 ? 1 : cols;
}

static void editor_clamp_view(window_t* w) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    uint32_t len;
    int rows = editor_visible_rows(w);
    int cols = editor_visible_cols(w);
    if (editor->model.count == 0) {
        editor->row = 0;
        editor->column = 0;
    } else if (editor->row >= editor->model.count) {
        editor->row = editor->model.count - 1u;
    }
    len = editor_model_line_length(&editor->model, editor->row);
    if (editor->column > len) editor->column = len;
    if (editor->row < editor->top) editor->top = editor->row;
    if (editor->row >= editor->top + (uint32_t)rows)
        editor->top = editor->row - (uint32_t)rows + 1u;
    if (editor->column < editor->hscroll) editor->hscroll = editor->column;
    if (editor->column >= editor->hscroll + (uint32_t)cols)
        editor->hscroll = editor->column - (uint32_t)cols + 1u;
}

static gui_rect_t editor_confirm_button(const window_t* w, int choice) {
    int panel_w = 244;
    int panel_x = w->x + (w->w - panel_w) / 2;
    int panel_y = w->y + TITLE_H + (w->h - TITLE_H - 74) / 2;
    return make_rect(panel_x + 8 + (choice - 1) * 76,
                     panel_y + 42, 68, 20);
}

static void editor_app_open(window_t* w, const char* argument) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    editor_model_init(&editor->model, argument ? argument : "");
    editor->caret_visible = 1;
    editor->confirm_choice = EDITOR_WIDGET_SAVE;
    editor->next_blink = sys_get_ticks() + SMALLOS_TIMER_HZ / 2u;
    if (!editor_model_load(&editor->model))
        editor_set_status(editor, "Unable to load file");
    else
        editor_set_status(editor, "F2 or Ctrl+S saves");
}

static void editor_app_close(window_t* w) {
    if (w->state) editor_model_destroy(&EDITOR_STATE(w)->model);
}

static void editor_app_draw(gfx_surface_t* s, window_t* w, int mx, int my) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    int by = w->y + TITLE_H;
    int rows = editor_visible_rows(w);
    int cols = editor_visible_cols(w);
    int text_y = by + EDITOR_TOOLBAR_H + 2;
    char location[64];
    char number[16];
    (void)mx;
    (void)my;
    fillr(s, w->x, by, w->w, w->h - TITLE_H, 0x00FFFFFFu);
    draw_fixed_text(s, w->x + 4, by + 4, editor->model.path,
                    cols, COL_TEXT);
    hline(s, w->x, by + EDITOR_TOOLBAR_H, w->w, COL_FRAME);
    for (int i = 0; i < rows; i++) {
        uint32_t row = editor->top + (uint32_t)i;
        uint32_t selection_start = 0, selection_end = 0;
        int row_selected = 0;
        if (row >= editor->model.count) break;
        if (editor_has_selection(editor)) {
            uint32_t first_row, first_column, last_row, last_column;
            editor_selection_bounds(editor, &first_row, &first_column,
                                    &last_row, &last_column);
            if (row >= first_row && row <= last_row) {
                uint32_t line_len = editor_model_line_length(&editor->model,
                                                              row);
                selection_start = row == first_row ? first_column : 0u;
                selection_end = row == last_row ? last_column : line_len + 1u;
                row_selected = selection_end > selection_start;
                if (row_selected) {
                    uint32_t visible_start = selection_start > editor->hscroll ?
                        selection_start : editor->hscroll;
                    uint32_t visible_end = selection_end < editor->hscroll +
                        (uint32_t)cols ? selection_end :
                        editor->hscroll + (uint32_t)cols;
                    if (visible_end > visible_start)
                        fillr(s, w->x + 4 +
                              (int)(visible_start - editor->hscroll) * 6,
                              text_y + i * EDITOR_LINE_H,
                              (int)(visible_end - visible_start) * 6,
                              7, COL_HILIGHT);
                }
            }
        }
        draw_fixed_text(s, w->x + 4, text_y + i * EDITOR_LINE_H,
                        editor->model.lines[row] +
                            (editor->hscroll < editor_model_line_length(
                                &editor->model, row) ? editor->hscroll :
                                editor_model_line_length(&editor->model, row)),
                        cols, COL_TEXT);
        if (row_selected) {
            uint32_t line_len = editor_model_line_length(&editor->model, row);
            uint32_t visible_start = selection_start > editor->hscroll ?
                selection_start : editor->hscroll;
            uint32_t visible_end = selection_end < editor->hscroll +
                (uint32_t)cols ? selection_end : editor->hscroll +
                (uint32_t)cols;
            if (visible_end > line_len) visible_end = line_len;
            for (uint32_t column = visible_start; column < visible_end;
                 column++)
                draw_char(s, w->x + 4 +
                          (int)(column - editor->hscroll) * 6,
                          text_y + i * EDITOR_LINE_H,
                          editor->model.lines[row][column], COL_HILIGHT_T);
        }
    }
    if (editor->caret_visible && !editor->confirm_close &&
        editor->row >= editor->top &&
        editor->row < editor->top + (uint32_t)rows) {
        int caret_col = (int)(editor->column - editor->hscroll);
        if (caret_col >= 0 && caret_col < cols)
            vline(s, w->x + 4 + caret_col * 6,
                  text_y + ((int)editor->row - (int)editor->top) * EDITOR_LINE_H,
                  7, COL_TITLE_BG);
    }
    hline(s, w->x, w->y + w->h - EDITOR_STATUS_H, w->w, COL_FRAME);
    draw_text(s, w->x + 4, w->y + w->h - 10,
              editor->model.dirty ? "*" : " ", COL_TEXT);
    u_strcpy_n(location, "Ln ", sizeof(location));
    utoa10(editor->row + 1u, number);
    u_strcat_n(location, number, sizeof(location));
    u_strcat_n(location, " Col ", sizeof(location));
    utoa10(editor->column + 1u, number);
    u_strcat_n(location, number, sizeof(location));
    u_strcat_n(location, "  ", sizeof(location));
    u_strcat_n(location, editor->status, sizeof(location));
    draw_text(s, w->x + 16, w->y + w->h - 10, location, COL_SUBTEXT);
    gui_widget_scrollbar(s,
        make_rect(w->x + w->w - 10, text_y, 10, rows * EDITOR_LINE_H),
        (int)editor->model.count, rows, (int)editor->top,
        (gui_widget_state_t){0,
            w->pressed_widget == EDITOR_WIDGET_SCROLLBAR, 0, 0},
        &GUI_WIDGET_THEME);

    if (editor->confirm_close) {
        int panel_w = 244;
        int panel_x = w->x + (w->w - panel_w) / 2;
        int panel_y = w->y + TITLE_H + (w->h - TITLE_H - 74) / 2;
        fillr(s, panel_x, panel_y, panel_w, 74, COL_WIN_BG);
        rect(s, panel_x, panel_y, panel_w, 74, COL_FRAME);
        draw_text(s, panel_x + 8, panel_y + 10,
                  "Save changes before closing?", COL_TEXT);
        for (int choice = 1; choice <= 3; choice++) {
            gui_widget_state_t state = {w->focused_widget == choice,
                w->pressed_widget == choice,
                editor->confirm_choice == choice, 0};
            const char* label = choice == 1 ? "Save" :
                                choice == 2 ? "Discard" : "Cancel";
            gui_widget_button(s, editor_confirm_button(w, choice), label,
                              state, &GUI_WIDGET_THEME, draw_text);
        }
    }
    rect(s, w->x, w->y, w->w, w->h, COL_FRAME);
}

static unsigned int editor_confirm_event(window_t* w,
                                         const gui_app_event_t* event) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    if (event->type == GUI_APP_EVENT_POINTER_MOVE) {
        int sx = w->x + event->x;
        int sy = w->y + TITLE_H + event->y;
        int hovered = 0;
        for (int choice = 1; choice <= 3; choice++) {
            if (gui_widget_hit(editor_confirm_button(w, choice), sx, sy))
                hovered = choice;
        }
        if (hovered != w->focused_widget) {
            w->focused_widget = hovered;
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        return GUI_APP_RESULT_HANDLED;
    }
    if (event->type == GUI_APP_EVENT_POINTER_DOWN) {
        int sx = w->x + event->x;
        int sy = w->y + TITLE_H + event->y;
        for (int choice = 1; choice <= 3; choice++) {
            if (gui_widget_hit(editor_confirm_button(w, choice), sx, sy)) {
                editor->confirm_choice = choice;
                w->focused_widget = choice;
                w->pressed_widget = choice;
                break;
            }
        }
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP) {
        int sx = w->x + event->x;
        int sy = w->y + TITLE_H + event->y;
        int pressed = w->pressed_widget;
        w->pressed_widget = 0;
        if (pressed >= 1 && pressed <= 3 &&
            gui_widget_hit(editor_confirm_button(w, pressed), sx, sy)) {
            editor->confirm_choice = pressed;
            if (pressed == EDITOR_WIDGET_SAVE) {
                if (editor_model_save(&editor->model))
                    return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_CLOSE;
                editor_set_status(editor, "Save failed");
                editor->confirm_close = 0;
            } else if (pressed == EDITOR_WIDGET_DISCARD) {
                editor->model.dirty = 0;
                return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_CLOSE;
            } else {
                editor->confirm_close = 0;
            }
        }
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type != GUI_APP_EVENT_KEY)
        return GUI_APP_RESULT_HANDLED;
    if (event->key == KEY_ESC) {
        editor->confirm_close = 0;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->key == KEY_LEFT || event->key == KEY_UP) {
        editor->confirm_choice--;
        if (editor->confirm_choice < 1) editor->confirm_choice = 3;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->key == KEY_RIGHT || event->key == KEY_DOWN ||
        event->key == KEY_TAB) {
        editor->confirm_choice++;
        if (editor->confirm_choice > 3) editor->confirm_choice = 1;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->key == KEY_ENTER) {
        if (editor->confirm_choice == EDITOR_WIDGET_SAVE) {
            if (editor_model_save(&editor->model))
                return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_CLOSE;
            editor_set_status(editor, "Save failed");
            editor->confirm_close = 0;
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
        }
        if (editor->confirm_choice == EDITOR_WIDGET_DISCARD)
        {
            editor->model.dirty = 0;
            return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_CLOSE;
        }
        editor->confirm_close = 0;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    return GUI_APP_RESULT_HANDLED;
}

static void editor_pointer_position(window_t* w, int x, int y,
                                    uint32_t* row, uint32_t* column) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    int text_y = EDITOR_TOOLBAR_H + 2;
    int visual_row = (y - text_y) / EDITOR_LINE_H;
    if (y < text_y) visual_row = 0;
    if (visual_row >= editor_visible_rows(w))
        visual_row = editor_visible_rows(w) - 1;
    *row = editor->top + (uint32_t)visual_row;
    if (editor->model.count == 0) *row = 0;
    else if (*row >= editor->model.count) *row = editor->model.count - 1u;
    *column = editor->hscroll +
        (uint32_t)((x > 4 ? x - 4 : 0) / 6);
    if (*column > editor_model_line_length(&editor->model, *row))
        *column = editor_model_line_length(&editor->model, *row);
}

static unsigned int editor_app_event(window_t* w,
                                     const gui_app_event_t* event) {
    editor_window_state_t* editor = EDITOR_STATE(w);
    uint32_t len;
    int selection_deleted = 0;
    int rows = editor_visible_rows(w);
    gui_rect_t scroll_track = make_rect(w->w - 10,
        EDITOR_TOOLBAR_H + 2, 10, rows * EDITOR_LINE_H);
    gui_rect_t scroll_thumb = gui_widget_scroll_thumb(scroll_track,
        (int)editor->model.count, rows, (int)editor->top);
    if (editor->confirm_close) return editor_confirm_event(w, event);
    if (event->type == GUI_APP_EVENT_CLOSE_REQUEST) {
        if (!editor->model.dirty) return GUI_APP_RESULT_CLOSE;
        editor->confirm_close = 1;
        editor->confirm_choice = EDITOR_WIDGET_SAVE;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW |
               GUI_APP_RESULT_KEEP_OPEN;
    }
    if (event->type == GUI_APP_EVENT_TICK) {
        editor->caret_visible = !editor->caret_visible;
        return GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_RESIZE) {
        editor_clamp_view(w);
        return GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_WHEEL) {
        int max_top = (int)editor->model.count - rows;
        int top = (int)editor->top - event->wheel * 3;
        if (max_top < 0) max_top = 0;
        if (top < 0) top = 0;
        if (top > max_top) top = max_top;
        editor->top = (uint32_t)top;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_MOVE &&
        w->pressed_widget == EDITOR_WIDGET_SCROLLBAR) {
        editor->top = (uint32_t)gui_widget_scroll_offset(
            scroll_track, (int)editor->model.count, rows, event->y,
            editor->scroll_drag_offset);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_MOVE &&
        w->pressed_widget == EDITOR_WIDGET_TEXT) {
        editor_pointer_position(w, event->x, event->y,
                                &editor->row, &editor->column);
        editor->selection_active = 1;
        editor->caret_visible = 1;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_UP &&
        (w->pressed_widget == EDITOR_WIDGET_SCROLLBAR ||
         w->pressed_widget == EDITOR_WIDGET_TEXT)) {
        if (w->pressed_widget == EDITOR_WIDGET_TEXT &&
            !editor_has_selection(editor)) editor_clear_selection(editor);
        w->pressed_widget = 0;
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type == GUI_APP_EVENT_POINTER_DOWN) {
        if (gui_widget_hit(scroll_track, event->x, event->y)) {
            if (gui_widget_hit(scroll_thumb, event->x, event->y)) {
                w->pressed_widget = EDITOR_WIDGET_SCROLLBAR;
                editor->scroll_drag_offset = event->y - scroll_thumb.y;
            } else {
                int max_top = (int)editor->model.count - rows;
                int top = (int)editor->top +
                    (event->y < scroll_thumb.y ? -rows : rows);
                if (max_top < 0) max_top = 0;
                if (top < 0) top = 0;
                if (top > max_top) top = max_top;
                editor->top = (uint32_t)top;
            }
        } else if (event->y >= EDITOR_TOOLBAR_H + 2 &&
                   event->y < EDITOR_TOOLBAR_H + 2 +
                       rows * EDITOR_LINE_H) {
            editor_pointer_position(w, event->x, event->y,
                                    &editor->row, &editor->column);
            editor->anchor_row = editor->row;
            editor->anchor_column = editor->column;
            editor->selection_active = 1;
            w->pressed_widget = EDITOR_WIDGET_TEXT;
            editor->caret_visible = 1;
        }
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if (event->type != GUI_APP_EVENT_KEY) return GUI_APP_RESULT_NONE;
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->key == KEY_S || event->ascii == 's' || event->ascii == 'S')) {
        editor_set_status(editor, editor_model_save(&editor->model)
            ? "Saved" : "Save failed");
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->key == KEY_A || event->ascii == 'a' || event->ascii == 'A')) {
        if (editor->model.count) {
            editor->anchor_row = 0;
            editor->anchor_column = 0;
            editor->row = editor->model.count - 1u;
            editor->column = editor_model_line_length(&editor->model,
                                                       editor->row);
            editor->selection_active = 1;
            editor_clamp_view(w);
        }
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->key == KEY_C || event->ascii == 'c' || event->ascii == 'C')) {
        editor_set_status(editor, editor_copy_selection(editor) ?
                          "Copied" : "Nothing selected");
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->key == KEY_X || event->ascii == 'x' || event->ascii == 'X')) {
        if (editor_copy_selection(editor) && editor_delete_selection(editor))
            editor_set_status(editor, "Cut");
        else
            editor_set_status(editor, "Nothing selected");
        editor_clamp_view(w);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    if ((event->modifiers & SYS_INPUT_KEY_CTRL) &&
        (event->key == KEY_V || event->ascii == 'v' || event->ascii == 'V')) {
        editor_set_status(editor, editor_paste(editor) ?
                          "Pasted" : "Clipboard empty");
        editor_clamp_view(w);
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
    }
    {
        int navigation = event->key == KEY_UP || event->key == KEY_DOWN ||
            event->key == KEY_LEFT || event->key == KEY_RIGHT ||
            event->key == KEY_HOME || event->key == KEY_END ||
            event->key == KEY_PAGEUP || event->key == KEY_PAGEDOWN;
        if (navigation && (event->modifiers & SYS_INPUT_KEY_SHIFT)) {
            if (!editor->selection_active) {
                editor->anchor_row = editor->row;
                editor->anchor_column = editor->column;
                editor->selection_active = 1;
            }
        } else if (navigation) {
            editor_clear_selection(editor);
        } else if (event->key == KEY_BACKSPACE || event->key == KEY_DELETE ||
                   event->key == KEY_ENTER || event->key == KEY_TAB ||
                   (event->ascii & 0xFFu) >= 32u) {
            selection_deleted = editor_delete_selection(editor);
        }
    }
    if (event->key == KEY_F2) {
        editor_set_status(editor, editor_model_save(&editor->model)
            ? "Saved" : "Save failed");
    } else if (event->key == KEY_ESC) {
        return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_CLOSE;
    } else if (event->key == KEY_UP) {
        if (editor->row > 0) editor->row--;
    } else if (event->key == KEY_DOWN) {
        if (editor->row + 1u < editor->model.count) editor->row++;
    } else if (event->key == KEY_LEFT) {
        if (editor->column > 0) editor->column--;
        else if (editor->row > 0) {
            editor->row--;
            editor->column = editor_model_line_length(&editor->model,
                                                       editor->row);
        }
    } else if (event->key == KEY_RIGHT) {
        len = editor_model_line_length(&editor->model, editor->row);
        if (editor->column < len) editor->column++;
        else if (editor->row + 1u < editor->model.count) {
            editor->row++;
            editor->column = 0;
        }
    } else if (event->key == KEY_HOME) {
        editor->column = 0;
    } else if (event->key == KEY_END) {
        editor->column = editor_model_line_length(&editor->model, editor->row);
    } else if (event->key == KEY_PAGEUP) {
        uint32_t amount = (uint32_t)editor_visible_rows(w);
        editor->row = amount > editor->row ? 0 : editor->row - amount;
    } else if (event->key == KEY_PAGEDOWN) {
        editor->row += (uint32_t)editor_visible_rows(w);
        if (editor->model.count && editor->row >= editor->model.count)
            editor->row = editor->model.count - 1u;
    } else if (event->key == KEY_BACKSPACE && !selection_deleted) {
        (void)editor_model_backspace(&editor->model, &editor->row,
                                     &editor->column);
    } else if (event->key == KEY_DELETE && !selection_deleted) {
        (void)editor_model_delete(&editor->model, editor->row, editor->column);
    } else if ((event->key == KEY_BACKSPACE || event->key == KEY_DELETE) &&
               selection_deleted) {
        /* The selection deletion is the complete key action. */
    } else if (event->key == KEY_ENTER) {
        if (editor_model_split_line(&editor->model, editor->row,
                                    editor->column)) {
            editor->row++;
            editor->column = 0;
        }
    } else if (event->key == KEY_TAB) {
        for (int i = 0; i < 4; i++) {
            if (!editor_model_insert_char(&editor->model, editor->row,
                                          editor->column, ' ')) break;
            editor->column++;
        }
    } else if ((event->ascii & 0xFFu) >= 32u) {
        if (editor_model_insert_char(&editor->model, editor->row,
                                     editor->column,
                                     (char)(event->ascii & 0xFFu)))
            editor->column++;
        else
            editor_set_status(editor, "Line too long");
    } else {
        return GUI_APP_RESULT_NONE;
    }
    editor->caret_visible = 1;
    editor_clamp_view(w);
    return GUI_APP_RESULT_HANDLED | GUI_APP_RESULT_REDRAW;
}

static const window_app_ops_t WINDOW_APPS[] = {
    {0},
    {"Files", sizeof(files_window_state_t), 360, 240, 220, 120, 0,
     files_app_open, 0, files_app_draw, files_app_event},
    {"System", 0, 280, 200, 200, 120, 0,
     0, 0, system_app_draw, 0},
    {"Config", 0, 260, 116, 220, 100, 0,
     0, 0, config_app_draw, config_app_event},
    {"About", 0, 280, 140, 220, 100, 0,
     0, 0, about_app_draw, 0},
    {"Shell", sizeof(gui_shell_window_t), 500, 320, 240, 120,
     GUI_SHELL_POLL_TICKS, shell_app_open, shell_app_close,
     shell_app_draw, shell_app_event},
    {"Editor", sizeof(editor_window_state_t), 560, 380, 260, 140,
     SMALLOS_TIMER_HZ / 2u, editor_app_open, editor_app_close,
     editor_app_draw, editor_app_event},
};

static const window_app_ops_t* window_app_ops(win_type_t type) {
    if ((unsigned int)type >= sizeof(WINDOW_APPS) / sizeof(WINDOW_APPS[0]))
        return 0;
    return &WINDOW_APPS[type];
}

static unsigned int dispatch_app_event(window_t* w,
                                       const gui_app_event_t* event) {
    const window_app_ops_t* ops = w ? window_app_ops(w->type) : 0;
    if (!w || !w->active || !ops || !ops->event) return GUI_APP_RESULT_NONE;
    return ops->event(w, event);
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

static void draw_window(gfx_surface_t* s, window_t* w, int focused, int mx, int my) {
    const window_app_ops_t* ops = window_app_ops(w->type);
    /* drop shadow */
    fillr(s, w->x + 3, w->y + w->h, w->w, 3, COL_SHADOW);
    fillr(s, w->x + w->w, w->y + 3, 3, w->h, COL_SHADOW);

    fillr(s, w->x, w->y, w->w, w->h, COL_WIN_BG);
    draw_title_bar(s, w, focused, ops ? ops->title : "");
    if (ops && ops->draw) ops->draw(s, w, mx, my);

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
    if (win->y > sh - TITLE_H) win->y = sh - TITLE_H;
    if (ops->open) ops->open(win, argument);
    return win;
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

static void action_quit(int sw, int sh) {
    int can_quit = 1;
    for (int i = MAX_WINDOWS - 1; i >= 0; i--) {
        if (g_wins[i].active && !request_window_close(&g_wins[i], sw, sh))
            can_quit = 0;
    }
    if (can_quit) g_should_quit = 1;
}

#define ICON_COUNT 6
static icon_t g_icons[ICON_COUNT];

static void icons_layout(int sw) {
    int x = sw - 80;
    int y = 30;
    g_icons[0].x = x; g_icons[0].y = y;       g_icons[0].label = "Files";
    g_icons[0].draw = icon_files;             g_icons[0].action = action_files;
    g_icons[1].x = x; g_icons[1].y = y + 60;  g_icons[1].label = "Shell";
    g_icons[1].draw = icon_terminal;          g_icons[1].action = action_shell;
    g_icons[2].x = x; g_icons[2].y = y + 120; g_icons[2].label = "System";
    g_icons[2].draw = icon_system;            g_icons[2].action = action_system;
    g_icons[3].x = x; g_icons[3].y = y + 180; g_icons[3].label = "Config";
    g_icons[3].draw = icon_config;            g_icons[3].action = action_config;
    g_icons[4].x = x; g_icons[4].y = y + 240; g_icons[4].label = "About";
    g_icons[4].draw = icon_about;             g_icons[4].action = action_about;
    g_icons[5].x = x; g_icons[5].y = y + 300; g_icons[5].label = "Quit";
    g_icons[5].draw = icon_quit;              g_icons[5].action = action_quit;
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

/* ---------------- background + top bar ---------------- */

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

static void draw_top_bar(gfx_surface_t* s) {
    int w = (int)s->width;
    fillr(s, 0, 0, w, 14, COL_TOPBAR);
    hline(s, 0, 14, w, COL_FRAME);

    char buf[96], num[16];
    sys_fsinfo_t fs;
    if (sys_fsinfo(&fs) == 0) {
        u_strcpy_n(buf, "Free: ", sizeof(buf));
        utoa10(fs.free_bytes / 1024u, num);
        u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, " KB", sizeof(buf));
        draw_text(s, 8, 4, buf, COL_TEXT);
    }

    u_strcpy_n(buf, "ESC/Q", sizeof(buf));
    if (g_perf_visible) {
        u_strcat_n(buf, "  GUI ", sizeof(buf));
        utoa10(g_perf.shown_presents, num);
        u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, "u ", sizeof(buf));
        utoa10(g_perf.shown_input_events, num);
        u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, "i max ", sizeof(buf));
        utoa10(g_perf.shown_max_loop_gap, num);
        u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, "/", sizeof(buf));
        utoa10(g_perf.shown_max_present_ticks, num);
        u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, "t", sizeof(buf));
    }
    draw_text(s, w - 8 - (int)text_width(buf), 4, buf, COL_TEXT);
}

/* ---------------- compose ---------------- */

static void compose_v2(gfx_surface_t* s, int mx, int my) {
    clip_clear();
    draw_desktop(s);
    draw_top_bar(s);

    int hi = icon_hit(mx, my);
    if (hit_window_z(mx, my)) hi = -1;
    draw_icons(s, hi);

    window_t* top = topmost();
    for (int i = 0; i < gui_window_stack_count(&g_window_stack); i++) {
        window_t* w = &g_wins[gui_window_stack_at(&g_window_stack, i)];
        if (!w->active) continue;
        draw_window(s, w, w == top, mx, my);
    }
}

static int present_cursor_rect(gfx_context_t* gfx, int mx, int my, int draw);

static void compose_rect(gfx_surface_t* s, gui_rect_t r, int mx, int my) {
    clip_set(r);
    draw_desktop(s);

    if (rect_intersects(r, make_rect(0, 0, (int)s->width, 15))) {
        draw_top_bar(s);
    }

    int hi = icon_hit(mx, my);
    if (hit_window_z(mx, my)) hi = -1;
    draw_icons(s, hi);

    window_t* top = topmost();
    for (int i = 0; i < gui_window_stack_count(&g_window_stack); i++) {
        window_t* w = &g_wins[gui_window_stack_at(&g_window_stack, i)];
        if (!w->active) continue;
        if (!rect_intersects(r, window_screen_rect(w))) continue;
        draw_window(s, w, w == top, mx, my);
    }
    clip_clear();
}

static int present_dirty_scene(gfx_context_t* gfx, int mx, int my) {
    gui_rect_t cursor = cursor_screen_rect(mx, my);
    int sw = (int)gfx->backbuffer.width;
    int sh = (int)gfx->backbuffer.height;

    for (int i = 0; i < g_dirty_count; i++) {
        gui_rect_t r = rect_clip_screen(g_dirty[i], sw, sh);
        if (rect_empty(r)) continue;
        compose_rect(&gfx->backbuffer, r, mx, my);
        {
            uint32_t present_start = sys_get_ticks();
            if (gfx_present_rect(gfx, (unsigned int)r.x, (unsigned int)r.y,
                                 (unsigned int)r.w, (unsigned int)r.h) < 0) {
                return -1;
            }
            perf_note_present(present_start);
        }
        if (rect_intersects(r, cursor)) {
            if (present_cursor_rect(gfx, mx, my, 1) < 0) {
                return -1;
            }
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

    if (!gfx || !gfx->backbuffer.pixels || !gfx->presentbuffer.pixels) return -1;
    sw = (int)gfx->backbuffer.width;
    sh = (int)gfx->backbuffer.height;
    r = rect_clip_screen(r, sw, sh);
    if (rect_empty(r)) return 0;
    if ((unsigned int)r.w * (unsigned int)r.h >
        gfx->presentbuffer.width * gfx->presentbuffer.height) {
        return -1;
    }
    tmp = gfx->presentbuffer.pixels;

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

static int apps_have_timers(void) {
    for (int i = 0; i < MAX_WINDOWS; i++) {
        const window_app_ops_t* ops;
        if (!g_wins[i].active) continue;
        ops = window_app_ops(g_wins[i].type);
        if (ops && ops->tick_interval) return 1;
    }
    return 0;
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
        if (result & GUI_APP_RESULT_REDRAW) dirty = 1;
        if (w->active) w->next_tick = now + ops->tick_interval;
    }
    return dirty;
}

static uint32_t gui_wait_deadline(uint32_t now) {
    uint32_t deadline = 0;
    int have_deadline = 0;

    if (gui_frame_pending() && !gui_frame_due(now)) {
        deadline = gui_frame_deadline();
        have_deadline = 1;
    }

    for (int i = 0; i < MAX_WINDOWS; i++) {
        const window_app_ops_t* ops;
        if (!g_wins[i].active) continue;
        ops = window_app_ops(g_wins[i].type);
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

    if (w->type == WT_FILES) {
        files_window_state_t* files = FILES_STATE(w);
        int by = w->y + TITLE_H;
        int row_top = by + 14;
        int row_area = (w->h - TITLE_H) - 27;
        int visible = row_area / ROW_H;
        if (visible < 1) visible = 1;
        if (mx >= w->x && mx < w->x + w->w - 12 &&
            my >= row_top && my < row_top + visible * ROW_H) {
            int row = (my - row_top) / ROW_H + files->scroll;
            return 2000 + win_index(w) * 512 + row;
        }
    }

    return 3000 + win_index(w);
}

static int invalidate_hover_key(int sw, int sh, int key) {
    if (key >= 1000 && key < 1000 + ICON_COUNT) {
        int idx = key - 1000;
        invalidate_rect(sw, sh, make_rect(g_icons[idx].x - 2, g_icons[idx].y - 2, 36, 44));
        return 1;
    }

    if (key >= 2000 && key < 3000) {
        int encoded = key - 2000;
        int widx = encoded / 512;
        int row = encoded % 512;
        if (widx >= 0 && widx < MAX_WINDOWS) {
            window_t* w = &g_wins[widx];
            if (w->active && w->type == WT_FILES) {
                files_window_state_t* files = FILES_STATE(w);
                int by = w->y + TITLE_H;
                int row_top = by + 14;
                int ry = row_top + (row - files->scroll) * ROW_H;
                invalidate_rect(sw, sh, make_rect(w->x, ry, w->w - 12, ROW_H));
                return 1;
            }
        }
    }

    return 0;
}

static void handle_click(int mx, int my, gui_fp_t mxf, gui_fp_t myf, int sw, int sh) {
    /* close button on any window? */
    window_t* w = hit_window_z(mx, my);
    if (w) {
        int cx = w->x + w->w - CLOSE_W - 2;
        int cy = w->y + 2;
        if (point_in(mx, my, cx, cy, CLOSE_W - 2, TITLE_H - 4)) {
            (void)request_window_close(w, sw, sh);
            return;
        }
        if (point_in(mx, my,
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
            window_t* old_top = topmost();
            if (old_top && old_top != w) invalidate_window(sw, sh, old_top);
            invalidate_window(sw, sh, w);
            z_push_top(win_index(w));
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

    /* drain any stale mouse delta */
    { sys_mouse_state_t m; (void)sys_mouse_read(&m); }

    int prev_left = 0;
    int dirty = 1;
    int cursor_dirty = 1;
    int presented_mx = mx;
    int presented_my = my;
    int last_hover = hover_key(mx, my);
    gui_config_load();
    if (argc > 1 && argv[1] && argv[1][0]) action_editor(sw, sh, argv[1]);
    invalidate_full(sw, sh);
    perf_init();

    while (!g_should_quit) {
        sys_input_event_t events[INPUT_BATCH];
        int got = 0;
        unsigned int input_flags = SYS_INPUT_FLAG_NONBLOCK;
        uint32_t loop_now = sys_get_ticks();
        int app_timers = apps_have_timers();
        int blocking_input_wait;

        if (!dirty &&
            !cursor_dirty &&
            g_launch_kind == LAUNCH_NONE &&
            !app_timers &&
            !gui_frame_pending()) {
            input_flags = 0;
        }
        blocking_input_wait = input_flags == 0;

        int n = read_input_coalesced(events, INPUT_BATCH, input_flags);
        if (blocking_input_wait) {
            perf_resume_from_idle();
        }
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
                    if (ev->wheel != 0) {
                        handle_wheel(mx, my, ev->wheel, sw, sh);
                        dirty = 1;
                    }
                    int left_now = (ev->buttons & SYS_MOUSE_BUTTON_LEFT) != 0;
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
                                                             16,
                                                             sh - TITLE_H);
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
                            int max_h = sh - w->y;
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
            if (present_dirty_scene(&gfx, mx, my) < 0) {
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
            if (deadline != 0u && !tick_due(now, deadline)) {
                (void)smallos_input_wait_until(deadline);
            } else {
                sys_yield();
            }
        }
    }

    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_wins[i].active) close_window(&g_wins[i]);
    }
    gfx_close(&gfx);
    u_puts("gui: exiting\n");
    return 0;
}
