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
 *   - "About" window: built-in static info (no fake screens)
 *   - "Quit" icon exits and releases the display
 *
 * ESC also exits.
 */

#include "user_lib.h"
#include "gfx.h"
#include "dirent.h"
#include "gui.h"
#include "shell_window.h"

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

static const unsigned char* glyph_for(char ch) {
    for (unsigned int i = 0; i < FONT_COUNT; i++) {
        if (FONT[i].ch == ch) return FONT[i].rows;
    }
    return 0;
}

static void draw_char(gfx_surface_t* s, int x, int y, char ch, unsigned int color) {
    const unsigned char* g = glyph_for(ch);
    if (!g) { gfx_fill_rect(s, (unsigned)x + 1, (unsigned)y + 6, 2, 1, color); return; }
    for (unsigned int row = 0; row < 7; row++) {
        unsigned int bits = g[row];
        for (unsigned int col = 0; col < 5; col++) {
            if (bits & (1u << (4u - col))) {
                gfx_put_pixel(s, (unsigned)x + col, (unsigned)y + row, color);
            }
        }
    }
}

static void draw_text(gfx_surface_t* s, int x, int y, const char* t, unsigned int c) {
    int cx = x;
    while (*t) { draw_char(s, cx, y, *t, c); cx += 6; t++; }
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
    if (!t || max_chars <= 0) return;
    while (end > 0 && t[end - 1] == ' ') end--;
    for (int i = 0; i < end && t[i]; i++) {
        draw_char(s, x + i * 6, y, t[i], c);
    }
}

/* ---------------- primitives ---------------- */

static void hline(gfx_surface_t* s, int x, int y, int w, unsigned int c) {
    if (w > 0) gfx_fill_rect(s, (unsigned)x, (unsigned)y, (unsigned)w, 1, c);
}
static void vline(gfx_surface_t* s, int x, int y, int h, unsigned int c) {
    if (h > 0) gfx_fill_rect(s, (unsigned)x, (unsigned)y, 1, (unsigned)h, c);
}
static void rect(gfx_surface_t* s, int x, int y, int w, int h, unsigned int c) {
    if (w <= 0 || h <= 0) return;
    hline(s, x, y, w, c); hline(s, x, y + h - 1, w, c);
    vline(s, x, y, h, c); vline(s, x + w - 1, y, h, c);
}
static void fillr(gfx_surface_t* s, int x, int y, int w, int h, unsigned int c) {
    if (w > 0 && h > 0) gfx_fill_rect(s, (unsigned)x, (unsigned)y, (unsigned)w, (unsigned)h, c);
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

#define MAX_WINDOWS 8
#define MAX_ROWS    256

typedef enum {
    WT_FILES = 1,
    WT_SYSTEM,
    WT_ABOUT,
    WT_SHELL,
} win_type_t;

typedef struct {
    int   active;
    int   z;            /* unused; kept for layout stability */
    win_type_t type;
    int   x, y, w, h;
    int   scroll;       /* row offset, FILES */
    /* FILES-specific */
    char  cwd[256];
    char  rows[MAX_ROWS][NAME_MAX + 1];
    int   row_dir[MAX_ROWS];
    int   row_count;
    int   need_reload;
    gui_shell_window_t shell;
} window_t;

static window_t g_wins[MAX_WINDOWS];

#define TITLE_H 14
#define CLOSE_W 14
#define ROW_H   12

/* z-order: g_zorder[0..count) holds indexes into g_wins, back to front. */
static int g_zorder[MAX_WINDOWS];
static int g_zorder_count = 0;

static int win_index(window_t* w) { return (int)(w - g_wins); }

static void z_remove_idx(int win_index_val) {
    int j = 0;
    for (int i = 0; i < g_zorder_count; i++) {
        if (g_zorder[i] != win_index_val) g_zorder[j++] = g_zorder[i];
    }
    g_zorder_count = j;
}

static void z_push_top(int win_index_val) {
    z_remove_idx(win_index_val);
    g_zorder[g_zorder_count++] = win_index_val;
}

static window_t* topmost(void) {
    if (g_zorder_count == 0) return 0;
    return &g_wins[g_zorder[g_zorder_count - 1]];
}

static window_t* hit_window_z(int mx, int my) {
    for (int i = g_zorder_count - 1; i >= 0; i--) {
        window_t* w = &g_wins[g_zorder[i]];
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
    if (!w) return;
    if (w->type == WT_SHELL) gui_shell_close(&w->shell);
    z_remove_idx(win_index(w));
    w->active = 0;
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

/* ---------------- file load ---------------- */

static void load_dir(window_t* w) {
    w->row_count = 0;
    w->scroll = 0;

    /* synthesize ".." for non-root */
    if (!u_streq(w->cwd, "/")) {
        u_strcpy_n(w->rows[w->row_count], "..", NAME_MAX + 1);
        w->row_dir[w->row_count] = 1;
        w->row_count++;
    }

    DIR* d = opendir(w->cwd);
    if (!d) {
        u_strcpy_n(w->rows[w->row_count], "<cannot open>", NAME_MAX + 1);
        w->row_dir[w->row_count] = 0;
        w->row_count++;
        return;
    }

    struct dirent* e;
    while ((e = readdir(d)) != 0 && w->row_count < MAX_ROWS) {
        u_strcpy_n(w->rows[w->row_count], e->d_name, NAME_MAX + 1);
        w->row_dir[w->row_count] = e->d_is_dir;
        w->row_count++;
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
        gfx_put_pixel(s, (unsigned)(ix + i), (unsigned)(iy + i), COL_FRAME);
        gfx_put_pixel(s, (unsigned)(ix + 5 - i), (unsigned)(iy + i), COL_FRAME);
    }
}

static void draw_files_body(gfx_surface_t* s, window_t* w, int mx, int my) {
    int bx = w->x;
    int by = w->y + TITLE_H;
    int bw = w->w;
    int bh = w->h - TITLE_H;
    fillr(s, bx, by, bw, bh, COL_WIN_BG);

    /* breadcrumb */
    draw_text(s, bx + 4, by + 2, "Path:", COL_SUBTEXT);
    draw_text(s, bx + 36, by + 2, w->cwd, COL_TEXT);
    hline(s, bx, by + 12, bw, COL_FRAME);

    int row_top = by + 14;
    int row_area = bh - 14;
    int visible = row_area / ROW_H;
    if (visible < 1) visible = 1;

    for (int i = 0; i < visible && (i + w->scroll) < w->row_count; i++) {
        int idx = i + w->scroll;
        int ry = row_top + i * ROW_H;
        int hover = (mx >= bx && mx < bx + bw - 12 && my >= ry && my < ry + ROW_H);
        if (hover) fillr(s, bx, ry, bw - 12, ROW_H, COL_HILIGHT);
        unsigned int color = hover ? COL_HILIGHT_T : COL_TEXT;
        /* tiny icon: [D] for dir, [F] for file */
        draw_text(s, bx + 4, ry + 2, w->row_dir[idx] ? "[D]" : "[F]", color);
        draw_text(s, bx + 28, ry + 2, w->rows[idx], color);
    }

    /* scrollbar */
    int sx = bx + bw - 10;
    fillr(s, sx, by + 14, 10, row_area, COL_BTN_BG);
    vline(s, sx, by + 14, row_area, COL_FRAME);
    if (w->row_count > visible) {
        int thumb_h = row_area * visible / w->row_count;
        if (thumb_h < 8) thumb_h = 8;
        int thumb_y = by + 14 + (row_area - thumb_h) * w->scroll /
                                 (w->row_count - visible);
        fillr(s, sx + 2, thumb_y, 6, thumb_h, COL_TITLE_BG);
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
    int bx = w->x;
    int by = w->y + TITLE_H;
    int bw = w->w;
    int bh = w->h - TITLE_H;
    fillr(s, bx, by, bw, bh, 0x00000000u);

    int line_h = 8;
    int pad = 4;
    int pty_mode = w->shell.backend == GUI_SHELL_BACKEND_PTY_CHILD;
    int input_h = pty_mode ? 0 : line_h + 2;
    int rows_area = bh - input_h - pad * 2;
    int visible = rows_area / line_h;
    if (visible < 1) visible = 1;
    int cols = (bw - 8) / 6;
    if (cols < 1) cols = 1;
    if (cols > GUI_SHELL_COLS) cols = GUI_SHELL_COLS;
    gui_shell_set_terminal_size(&w->shell, (unsigned int)visible, (unsigned int)cols);

    int start = w->shell.line_count - visible - w->shell.scroll;
    if (start < 0) start = 0;
    int ty = by + pad;
    for (int i = 0; i < visible; i++) {
        if ((start + i) >= w->shell.line_count) break;
        draw_fixed_text(s, bx + 4, ty, w->shell.lines[start + i], cols, 0x00C8C8C8u);
        ty += line_h;
    }

    if (pty_mode) {
        int cursor_row = w->shell.cursor_row - start;
        int cursor_col = w->shell.cursor_col;
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
    gui_shell_format_prompt(&w->shell, prompt, sizeof(prompt));
    draw_text(s, bx + 4, iy, prompt, 0x00FFFFFFu);
    /* cursor: a solid block right after input */
    int cursor_chars = (w->shell.backend == GUI_SHELL_BACKEND_PTY_CHILD)
        ? w->shell.pending_cursor
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

static const char* window_title(window_t* w) {
    switch (w->type) {
        case WT_FILES:    return "Files";
        case WT_SYSTEM:   return "System";
        case WT_ABOUT:    return "About";
        case WT_SHELL:    return "Shell";
    }
    return "";
}

static void draw_window(gfx_surface_t* s, window_t* w, int focused, int mx, int my) {
    /* drop shadow */
    fillr(s, w->x + 3, w->y + w->h, w->w, 3, COL_SHADOW);
    fillr(s, w->x + w->w, w->y + 3, 3, w->h, COL_SHADOW);

    fillr(s, w->x, w->y, w->w, w->h, COL_WIN_BG);
    draw_title_bar(s, w, focused, window_title(w));

    switch (w->type) {
        case WT_FILES:    draw_files_body(s, w, mx, my); break;
        case WT_SYSTEM:   draw_system_body(s, w); break;
        case WT_ABOUT:    draw_about_body(s, w); break;
        case WT_SHELL:    draw_shell_body(s, w); break;
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
        gfx_put_pixel(s, (unsigned)(x + 6 + i), (unsigned)(y + 6 + i), COL_FRAME);
        gfx_put_pixel(s, (unsigned)(x + 6 + i + 1), (unsigned)(y + 6 + i), COL_FRAME);
        gfx_put_pixel(s, (unsigned)(x + 6 + i), (unsigned)(y + 21 - i), COL_FRAME);
        gfx_put_pixel(s, (unsigned)(x + 6 + i + 1), (unsigned)(y + 21 - i), COL_FRAME);
    }
}

static int g_should_quit = 0;

static void action_files(int sw, int sh) {
    window_t* w = alloc_window();
    if (!w) return;
    w->type = WT_FILES;
    w->w = 360; w->h = 240;
    w->x = (sw - w->w) / 2;
    w->y = (sh - w->h) / 2;
    if (w->x < 4) w->x = 4;
    if (w->y < 20) w->y = 20;
    u_strcpy_n(w->cwd, "/", sizeof(w->cwd));
    load_dir(w);
}
static void action_system(int sw, int sh) {
    window_t* w = alloc_window();
    if (!w) return;
    w->type = WT_SYSTEM;
    w->w = 280; w->h = 200;
    w->x = (sw - w->w) / 2 + 40;
    w->y = (sh - w->h) / 2 - 20;
    if (w->x < 4) w->x = 4;
    if (w->y < 20) w->y = 20;
}
static void action_about(int sw, int sh) {
    window_t* w = alloc_window();
    if (!w) return;
    w->type = WT_ABOUT;
    w->w = 280; w->h = 140;
    w->x = (sw - w->w) / 2 - 40;
    w->y = (sh - w->h) / 2 + 20;
    if (w->x < 4) w->x = 4;
    if (w->y < 20) w->y = 20;
}
static void action_shell(int sw, int sh) {
    window_t* w = alloc_window();
    if (!w) return;
    w->type = WT_SHELL;
    w->w = 500; w->h = 320;
    w->x = (sw - w->w) / 2;
    w->y = (sh - w->h) / 2;
    if (w->x < 4) w->x = 4;
    if (w->y < 20) w->y = 20;
    gui_shell_open(&w->shell);
}

static void action_quit(int sw, int sh) {
    (void)sw; (void)sh;
    g_should_quit = 1;
}

#define ICON_COUNT 5
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
    g_icons[3].x = x; g_icons[3].y = y + 180; g_icons[3].label = "About";
    g_icons[3].draw = icon_about;             g_icons[3].action = action_about;
    g_icons[4].x = x; g_icons[4].y = y + 240; g_icons[4].label = "Quit";
    g_icons[4].draw = icon_quit;              g_icons[4].action = action_quit;
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
    for (unsigned int y = 0; y < s->height; y++) {
        unsigned int* row = s->pixels + y * s->pitch_pixels;
        for (unsigned int x = 0; x < s->width; x++) {
            row[x] = ((x ^ y) & 1u) ? COL_DESKTOP_A : COL_DESKTOP_B;
        }
    }
}

static void draw_top_bar(gfx_surface_t* s, int n_windows) {
    int w = (int)s->width;
    fillr(s, 0, 0, w, 14, COL_TOPBAR);
    hline(s, 0, 14, w, COL_FRAME);

    char buf[64], num[16];
    sys_fsinfo_t fs;
    if (sys_fsinfo(&fs) == 0) {
        u_strcpy_n(buf, "Free: ", sizeof(buf));
        utoa10(fs.free_bytes / 1024u, num);
        u_strcat_n(buf, num, sizeof(buf));
        u_strcat_n(buf, " KB", sizeof(buf));
        draw_text(s, 8, 4, buf, COL_TEXT);
    }
    u_strcpy_n(buf, "Windows: ", sizeof(buf));
    utoa10((unsigned int)n_windows, num);
    u_strcat_n(buf, num, sizeof(buf));
    draw_text(s, 200, 4, buf, COL_TEXT);

    const char* hint = "ESC or Q to exit";
    draw_text(s, w - 8 - (int)text_width(hint), 4, hint, COL_TEXT);
}

/* ---------------- cursor ---------------- */

static void draw_cursor(gfx_surface_t* s, int mx, int my) {
    static const unsigned char arrow[12][9] = {
        {1,0,0,0,0,0,0,0,0}, {1,1,0,0,0,0,0,0,0},
        {1,2,1,0,0,0,0,0,0}, {1,2,2,1,0,0,0,0,0},
        {1,2,2,2,1,0,0,0,0}, {1,2,2,2,2,1,0,0,0},
        {1,2,2,2,2,2,1,0,0}, {1,2,2,2,2,2,2,1,0},
        {1,2,2,2,1,1,1,1,0}, {1,2,1,2,2,1,0,0,0},
        {1,1,0,1,2,2,1,0,0}, {0,0,0,0,1,2,1,0,0},
    };
    for (int r = 0; r < 12; r++) {
        for (int c = 0; c < 9; c++) {
            int px = mx + c, py = my + r;
            if (px < 0 || py < 0) continue;
            if (arrow[r][c] == 1) gfx_put_pixel(s, (unsigned)px, (unsigned)py, 0);
            else if (arrow[r][c] == 2) gfx_put_pixel(s, (unsigned)px, (unsigned)py, 0xFFFFFFu);
        }
    }
}

/* ---------------- compose ---------------- */

static window_t* topmost_window(void) {
    window_t* top = 0;
    int best = -1;
    for (int i = 0; i < MAX_WINDOWS; i++) {
        if (g_wins[i].active && g_wins[i].z > best) {
            best = g_wins[i].z; top = &g_wins[i];
        }
    }
    return top;
}

static int count_windows(void) {
    int n = 0;
    for (int i = 0; i < MAX_WINDOWS; i++) if (g_wins[i].active) n++;
    return n;
}

static void compose_v2(gfx_surface_t* s, int mx, int my) {
    draw_desktop(s);
    draw_top_bar(s, count_windows());

    int hi = icon_hit(mx, my);
    if (hit_window_z(mx, my)) hi = -1;
    draw_icons(s, hi);

    window_t* top = topmost();
    for (int i = 0; i < g_zorder_count; i++) {
        window_t* w = &g_wins[g_zorder[i]];
        if (!w->active) continue;
        draw_window(s, w, w == top, mx, my);
    }

    draw_cursor(s, mx, my);
}

/* ---------------- input handling ---------------- */

static int clampi(int v, int lo, int hi) {
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

typedef enum {
    DRAG_NONE = 0,
    DRAG_MOVE,
} drag_mode_t;

static drag_mode_t g_drag = DRAG_NONE;
static int g_drag_idx = -1;
static int g_drag_dx = 0, g_drag_dy = 0;

static int point_in(int x, int y, int rx, int ry, int rw, int rh) {
    return x >= rx && x < rx + rw && y >= ry && y < ry + rh;
}

static void handle_click(int mx, int my, int sw, int sh) {
    /* close button on any window? */
    window_t* w = hit_window_z(mx, my);
    if (w) {
        int cx = w->x + w->w - CLOSE_W - 2;
        int cy = w->y + 2;
        if (point_in(mx, my, cx, cy, CLOSE_W - 2, TITLE_H - 4)) {
            close_window(w);
            return;
        }
        /* title bar: raise + start drag */
        if (point_in(mx, my, w->x, w->y, w->w, TITLE_H)) {
            z_push_top(win_index(w));
            g_drag = DRAG_MOVE;
            g_drag_idx = win_index(w);
            g_drag_dx = mx - w->x;
            g_drag_dy = my - w->y;
            return;
        }
        /* body click: raise */
        z_push_top(win_index(w));

        /* FILES: hit-test rows */
        if (w->type == WT_FILES) {
            int by = w->y + TITLE_H;
            int row_top = by + 14;
            int row_area = (w->h - TITLE_H) - 14;
            int visible = row_area / ROW_H;
            if (mx >= w->x && mx < w->x + w->w - 12 &&
                my >= row_top && my < row_top + visible * ROW_H) {
                int i = (my - row_top) / ROW_H;
                int idx = i + w->scroll;
                if (idx >= 0 && idx < w->row_count) {
                    if (w->row_dir[idx]) {
                        if (u_streq(w->rows[idx], "..")) {
                            path_normalize_parent(w->cwd);
                        } else {
                            path_append(w->cwd, w->rows[idx], sizeof(w->cwd));
                        }
                        load_dir(w);
                    }
                }
            }
            /* scrollbar hit: page up/down */
            int sx = w->x + w->w - 10;
            if (mx >= sx && mx < sx + 10) {
                if (my < (w->y + w->h / 2)) {
                    w->scroll -= visible;
                    if (w->scroll < 0) w->scroll = 0;
                } else {
                    int max_scroll = w->row_count - visible;
                    if (max_scroll < 0) max_scroll = 0;
                    w->scroll += visible;
                    if (w->scroll > max_scroll) w->scroll = max_scroll;
                }
            }
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

int gui_main(int argc, char** argv) {
    gfx_context_t gfx;
    int rc;

    (void)argc;
    (void)argv;

    u_puts("gui: starting (ESC or q to exit)\n");
    rc = gfx_open(&gfx);
    if (rc == -1) { u_puts("gui: framebuffer not available\n"); return 0; }
    if (rc < 0)   { u_puts("gui: could not open display\n");    return 1; }

    int sw = (int)gfx.backbuffer.width;
    int sh = (int)gfx.backbuffer.height;
    int mx = sw / 2;
    int my = sh / 2;

    icons_layout(sw);

    /* drain any stale mouse delta */
    { sys_mouse_state_t m; (void)sys_mouse_read(&m); }

    int prev_left = 0;
    int dirty = 1;

    while (!g_should_quit) {
        sys_input_event_t ev;
        int got = 0;

        for (;;) {
            int n = sys_input_read(&ev, 1u, SYS_INPUT_FLAG_NONBLOCK);
            if (n != 1) break;
            got = 1;

            if (ev.type == SYS_INPUT_TYPE_KEY &&
                (ev.flags & SYS_INPUT_KEY_PRESSED) != 0u) {
                unsigned int a = ev.ascii & 0xFFu;
                window_t* top = topmost();
                if (top && top->type == WT_SHELL) {
                    if (a == 27u) { close_window(top); }
                    else if (gui_shell_handle_key(&top->shell, a, ev.key, ev.flags) == GUI_SHELL_KEY_CLOSE) {
                        close_window(top);
                    }
                } else {
                    if (a == 27u || a == 'q' || a == 'Q') { g_should_quit = 1; break; }
                }
            } else if (ev.type == SYS_INPUT_TYPE_MOUSE) {
                mx = clampi(mx + ev.dx, 0, sw - 1);
                my = clampi(my + ev.dy, 0, sh - 1);
                int left_now = (ev.buttons & SYS_MOUSE_BUTTON_LEFT) != 0;
                if (left_now && !prev_left) {
                    handle_click(mx, my, sw, sh);
                } else if (!left_now && prev_left) {
                    g_drag = DRAG_NONE;
                    g_drag_idx = -1;
                }
                if (g_drag == DRAG_MOVE && g_drag_idx >= 0) {
                    window_t* w = &g_wins[g_drag_idx];
                    if (w->active) {
                        w->x = clampi(mx - g_drag_dx, -w->w + 32, sw - 32);
                        w->y = clampi(my - g_drag_dy, 16, sh - TITLE_H);
                    }
                }
                prev_left = left_now;
                dirty = 1;
            }
        }

        if (got) dirty = 1;

        for (int i = 0; i < MAX_WINDOWS; i++) {
            if (g_wins[i].active && g_wins[i].type == WT_SHELL) {
                if (gui_shell_poll(&g_wins[i].shell)) dirty = 1;
            }
        }

        if (dirty) {
            compose_v2(&gfx.backbuffer, mx, my);
            if (gfx_present(&gfx) < 0) {
                gfx_close(&gfx);
                u_puts("gui: present failed\n");
                return 1;
            }
            dirty = 0;
        }

        sys_sleep(1);
    }

    gfx_close(&gfx);
    u_puts("gui: exiting\n");
    return 0;
}
