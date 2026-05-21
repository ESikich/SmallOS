#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "gfx.h"
#include "gfx_indexed.h"
#include "gfx_text.h"
#include "port.h"
#include "prototyp.h"
#include "term_keys.h"

#define SMALLOS_KEY_F2 1060

extern int fractint_upstream_main(int argc, char** argv);
extern int keybuffer;
extern BYTE far* mapdacbox;
extern int colorpreloaded;

void setfortext(void);
void setforgraphics(void);

int fake_lut = 0;
int istruecolor = 0;
int daclearn = 0;
int dacnorm = 0;
int daccount = 0;
int ShadowColors = 0;
int goodmode = 0;
int andcolor = 255;
int diskflag = 0;
int videoflag = 0;
void (*swapsetup)(void) = NULL;
int color_dark = 0;
int color_bright = 255;
int color_medium = 128;
int reallyega = 0;
int gotrealdac = 1;
int rowcount = 0;
int video_type = 5;
int svga_type = 0;
int mode7text = 0;
int textaddr = 0xb800;
int textsafe = 0;
int text_type = 0;
int textrow = 0;
int textcol = 0;
int textrbase = 0;
int textcbase = 0;
int vesa_detect = 1;
int virtual = 0;
int video_scroll = 0;
int video_startx = 0;
int video_starty = 0;
int vesa_xres = 0;
int vesa_yres = 0;
int chkd_vvs = 0;
int video_vram = 0;
int screenctr = -1;
int unixDisk = 0;
char* Xdisplay = "";
char* Xgeometry = NULL;
char* Xmessage = NULL;
char* PSviewer = "";
int slowdisplay = 0;
int XZoomWaiting = 0;

struct videoinfo videotable[MAXVIDEOTABLE] = {
    {"SmallOS framebuffer       ", "                         ",
     SMALLOS_KEY_F2, 0, 0, 0, 0, 19, 1024, 768, 256},
    {"unused mode               ", "                         ",
     0, 0, 0, 0, 0, 0, 0, 0, 0},
};

static gfx_indexed_context_t s_indexed;
static int s_palette_ready;

#define TEXT_COLS 80
#define TEXT_ROWS 25
#define TEXT_CELL_W GFX_TEXT_DEFAULT_CELL_W
#define TEXT_CELL_H GFX_TEXT_DEFAULT_CELL_H
#define TEXT_STACK_MAX 3

static unsigned char s_text_chars[TEXT_ROWS][TEXT_COLS];
static unsigned short s_text_attrs[TEXT_ROWS][TEXT_COLS];
static int s_text_ready;
static int s_text_visible;

typedef struct text_snapshot {
    unsigned char chars[TEXT_ROWS][TEXT_COLS];
    unsigned short attrs[TEXT_ROWS][TEXT_COLS];
    int row;
    int col;
} text_snapshot_t;

static text_snapshot_t s_text_stack[TEXT_STACK_MAX];
static int s_text_stack_rc[TEXT_STACK_MAX + 1];

static unsigned int dac_channel_to_rgb(BYTE value) {
    unsigned int v = (unsigned int)value;

    if (v <= 63u) {
        return (v << 2) | (v >> 4);
    }
    return v & 255u;
}

static unsigned int dac_palette_color(int color) {
    unsigned int r;
    unsigned int g;
    unsigned int b;

    color &= 255;
    r = dac_channel_to_rgb(dacbox[color][0]);
    g = dac_channel_to_rgb(dacbox[color][1]);
    b = dac_channel_to_rgb(dacbox[color][2]);
    return (r << 16) | (g << 8) | b;
}

static void init_default_palette(void) {
    int i;

    if (s_palette_ready) {
        return;
    }
    if (mapdacbox || colorpreloaded) {
        s_palette_ready = 1;
        return;
    }
    for (i = 0; i < 256; i++) {
        dacbox[i][0] = (BYTE)((i >> 5) * 8 + 7);
        dacbox[i][1] = (BYTE)((((i + 16) & 28) >> 2) * 8 + 7);
        dacbox[i][2] = (BYTE)(((i + 2) & 3) * 16 + 15);
    }
    dacbox[0][0] = dacbox[0][1] = dacbox[0][2] = 0;
    dacbox[1][0] = dacbox[1][1] = dacbox[1][2] = 63;
    dacbox[2][0] = 47;
    dacbox[2][1] = 63;
    dacbox[2][2] = 63;
    s_palette_ready = 1;
}

static void sync_palette(void) {
    int i;

    init_default_palette();
    for (i = 0; i < 256; i++) {
        gfx_indexed_set_palette(&s_indexed, (unsigned int)i,
                                dac_palette_color(i));
    }
}

void smallos_graphics_batch_begin(void) {
    gfx_indexed_batch_begin(&s_indexed);
}

void smallos_graphics_batch_end(void) {
    gfx_indexed_batch_end(&s_indexed);
}

static void recolor_screen(void) {
    sync_palette();
    gfx_indexed_recolor(&s_indexed);
}

static void clear_graphics_pixels(BYTE color) {
    sync_palette();
    gfx_indexed_clear(&s_indexed, color);
}

static void flush_dirty(void) {
    gfx_indexed_flush(&s_indexed);
}

static int open_graphics(void) {
    unsigned int w;
    unsigned int h;

    if (gfx_indexed_is_open(&s_indexed)) {
        return 0;
    }
    if (gfx_indexed_open(&s_indexed) < 0) {
        return -1;
    }
    sync_palette();
    w = gfx_indexed_width(&s_indexed);
    h = gfx_indexed_height(&s_indexed);
    videotable[0].xdots = (int)w;
    videotable[0].ydots = (int)h;
    videotable[0].colors = 256;
    gfx_indexed_clear(&s_indexed, 0);
    return 0;
}

static void close_graphics(void) {
    gfx_indexed_close(&s_indexed);
}

static unsigned int text_origin_x(void) {
    unsigned int w = TEXT_COLS * TEXT_CELL_W;
    unsigned int screen_w = gfx_indexed_width(&s_indexed);

    if (!screen_w || screen_w <= w) {
        return 0;
    }
    return (screen_w - w) / 2u;
}

static unsigned int text_origin_y(void) {
    unsigned int h = TEXT_ROWS * TEXT_CELL_H;
    unsigned int screen_h = gfx_indexed_height(&s_indexed);

    if (!screen_h || screen_h <= h) {
        return 0;
    }
    return (screen_h - h) / 2u;
}

static void drain_pending_input(void) {
    keybuffer = 0;
    term_key_drain();
    keybuffer = 0;
}

static void text_init_buffer(void) {
    int row;
    int col;

    if (s_text_ready) {
        return;
    }
    for (row = 0; row < TEXT_ROWS; row++) {
        for (col = 0; col < TEXT_COLS; col++) {
            s_text_chars[row][col] = ' ';
            s_text_attrs[row][col] = (unsigned short)(BLACK * 16 + L_WHITE);
        }
    }
    s_text_ready = 1;
}

static int text_ensure_display(void) {
    text_init_buffer();
    if (open_graphics() < 0) {
        fprintf(stderr, "fractint: framebuffer display is not available\n");
        return -1;
    }
    s_text_visible = 1;
    return 0;
}

static void text_draw_cell(int row, int col) {
    unsigned int px0;
    unsigned int py0;
    gfx_surface_t* surface;

    surface = gfx_indexed_surface(&s_indexed);
    if (!surface || row < 0 || col < 0 ||
        row >= TEXT_ROWS || col >= TEXT_COLS) {
        return;
    }

    px0 = text_origin_x() + (unsigned int)col * TEXT_CELL_W;
    py0 = text_origin_y() + (unsigned int)row * TEXT_CELL_H;
    gfx_text_draw_cell(surface, px0, py0, TEXT_CELL_W, TEXT_CELL_H,
                       s_text_chars[row][col], s_text_attrs[row][col]);
}

static void text_present_cells(int row0, int col0, int row1, int col1) {
    unsigned int x;
    unsigned int y;
    unsigned int w;
    unsigned int h;

    if (!gfx_indexed_is_open(&s_indexed)) {
        return;
    }
    if (row0 < 0) row0 = 0;
    if (col0 < 0) col0 = 0;
    if (row1 >= TEXT_ROWS) row1 = TEXT_ROWS - 1;
    if (col1 >= TEXT_COLS) col1 = TEXT_COLS - 1;
    if (row1 < row0 || col1 < col0) {
        return;
    }
    if (row1 > row0) {
        col0 = 0;
        col1 = TEXT_COLS - 1;
    }

    x = text_origin_x() + (unsigned int)col0 * TEXT_CELL_W;
    y = text_origin_y() + (unsigned int)row0 * TEXT_CELL_H;
    w = (unsigned int)(col1 - col0 + 1) * TEXT_CELL_W;
    h = (unsigned int)(row1 - row0 + 1) * TEXT_CELL_H;
    gfx_indexed_present_rect(&s_indexed, x, y, w, h);
}

static void text_redraw_cells(int row0, int col0, int row1, int col1) {
    int row;
    int col;

    if (text_ensure_display() < 0) {
        return;
    }
    if (row0 < 0) row0 = 0;
    if (col0 < 0) col0 = 0;
    if (row1 >= TEXT_ROWS) row1 = TEXT_ROWS - 1;
    if (col1 >= TEXT_COLS) col1 = TEXT_COLS - 1;
    for (row = row0; row <= row1; row++) {
        for (col = col0; col <= col1; col++) {
            text_draw_cell(row, col);
        }
    }
    text_present_cells(row0, col0, row1, col1);
}

static void text_redraw_all(void) {
    gfx_surface_t* surface;

    if (text_ensure_display() < 0) {
        return;
    }
    surface = gfx_indexed_surface(&s_indexed);
    if (!surface) {
        return;
    }
    gfx_fill_rect(surface, 0, 0, surface->width, surface->height, 0);
    for (int row = 0; row < TEXT_ROWS; row++) {
        for (int col = 0; col < TEXT_COLS; col++) {
            text_draw_cell(row, col);
        }
    }
    gfx_indexed_present(&s_indexed);
}

static void text_clear(unsigned short attr) {
    int row;
    int col;

    text_init_buffer();
    for (row = 0; row < TEXT_ROWS; row++) {
        for (col = 0; col < TEXT_COLS; col++) {
            s_text_chars[row][col] = ' ';
            s_text_attrs[row][col] = attr;
        }
    }
    textrow = 0;
    textcol = 0;
    text_redraw_all();
}

static void text_scroll_range(int top, int bot) {
    int row;
    int col;

    text_init_buffer();
    if (top < 0) top = 0;
    if (bot >= TEXT_ROWS) bot = TEXT_ROWS - 1;
    if (bot <= top) {
        return;
    }
    for (row = top; row < bot; row++) {
        memcpy(s_text_chars[row], s_text_chars[row + 1], TEXT_COLS);
        memcpy(s_text_attrs[row], s_text_attrs[row + 1],
               TEXT_COLS * sizeof(s_text_attrs[0][0]));
    }
    for (col = 0; col < TEXT_COLS; col++) {
        s_text_chars[bot][col] = ' ';
        s_text_attrs[bot][col] = (unsigned short)(BLACK * 16 + L_WHITE);
    }
    text_redraw_cells(top, 0, bot, TEXT_COLS - 1);
}

int main(int argc, char** argv) {
    char* local_argv[48];
    int local_argc = 0;
    int saw_video = 0;
    int i;

    for (i = 0; i < argc && local_argc < 44; i++) {
        local_argv[local_argc++] = argv[i];
        if (strncmp(argv[i], "video=", 6) == 0) {
            saw_video = 1;
        }
    }
    if (!saw_video) {
        local_argv[local_argc++] = "video=F2";
    }
    if (argc == 1) {
        local_argv[local_argc++] = "type=mandel";
        local_argv[local_argc++] = "passes=1";
        local_argv[local_argc++] = "maxiter=150";
        local_argv[local_argc++] = "inside=0";
    }
    local_argv[local_argc] = NULL;
    drain_pending_input();
    return fractint_upstream_main(local_argc, local_argv);
}

int unixarg(int argc, char** argv, int* i) {
    if (*i < argc && strcmp(argv[*i], "-disk") == 0) {
        unixDisk = 1;
        return 1;
    }
    if (*i + 1 < argc && strcmp(argv[*i], "-display") == 0) {
        Xdisplay = argv[++(*i)];
        return 1;
    }
    if (*i + 1 < argc && strcmp(argv[*i], "-geometry") == 0) {
        Xgeometry = argv[++(*i)];
        return 1;
    }
    return 0;
}

void UnixInit(void) {
    init_default_palette();
}

void UnixDone(void) {
    close_graphics();
}

void initUnixWindow(void) {
    if (open_graphics() == 0) {
        sxdots = (int)gfx_indexed_width(&s_indexed);
        sydots = (int)gfx_indexed_height(&s_indexed);
        colors = 256;
        adapter = 0;
        videotable[0].xdots = sxdots;
        videotable[0].ydots = sydots;
        videotable[0].colors = colors;
    }
}

void adapter_detect(void) {
    video_type = 5;
}

void setvideotext(void) {
    dotmode = 0;
    text_clear((unsigned short)(BLACK * 16 + L_WHITE));
}

void setvideomode(int ax, int bx, int cx, int dx) {
    (void)ax;
    (void)bx;
    (void)cx;
    (void)dx;
    if (dotmode == 0) {
        goodmode = 0;
        text_clear((unsigned short)(BLACK * 16 + L_WHITE));
        return;
    }
    if (open_graphics() < 0) {
        fprintf(stderr, "fractint: framebuffer display is not available\n");
        exit(1);
    }
    sxdots = (int)gfx_indexed_width(&s_indexed);
    sydots = (int)gfx_indexed_height(&s_indexed);
    xdots = sxdots;
    ydots = sydots;
    colors = 256;
    andcolor = colors - 1;
    goodmode = 1;
    videoflag = 1;
    vesa_xres = sxdots;
    vesa_yres = sydots;
    rowcount = 0;
    s_text_visible = 0;
    init_default_palette();
    clear_graphics_pixels(0);
}

void setnullvideo(void) {
    goodmode = 0;
}

int getcolor(int xdot, int ydot) {
    int x = xdot + sxoffs;
    int y = ydot + syoffs;

    return gfx_indexed_get_pixel(&s_indexed, x, y);
}

void putcolor_a(int xdot, int ydot, int color) {
    int x = xdot + sxoffs;
    int y = ydot + syoffs;

    color &= andcolor;
    gfx_indexed_put_pixel(&s_indexed, x, y, (BYTE)color);
}

void get_line(int row, int startcol, int stopcol, BYTE* pixels) {
    int y = row + syoffs;

    gfx_indexed_get_line(&s_indexed, y, startcol + sxoffs,
                         stopcol + sxoffs, pixels);
}

void put_line(int row, int startcol, int stopcol, BYTE* pixels) {
    int y = row + syoffs;

    gfx_indexed_put_line(&s_indexed, y, startcol + sxoffs,
                         stopcol + sxoffs, pixels);
}

int out_line(BYTE* pixels, int linelen) {
    put_line(rowcount, 0, linelen - 1, pixels);
    rowcount++;
    return 0;
}

int readvideopalette(void) {
    init_default_palette();
    return 0;
}

int writevideopalette(void) {
    s_palette_ready = 1;
    find_special_colors();
    recolor_screen();
    return 0;
}

void find_special_colors(void) {
    int i;
    int best_dark = 10000;
    int best_bright = -1;
    int best_medium = 10000;

    init_default_palette();
    for (i = 0; i < colors && i < 256; i++) {
        int sum = (int)dacbox[i][0] + (int)dacbox[i][1] + (int)dacbox[i][2];
        int medium_dist = sum > 96 ? sum - 96 : 96 - sum;
        if (sum < best_dark) {
            best_dark = sum;
            color_dark = i;
        }
        if (sum > best_bright) {
            best_bright = sum;
            color_bright = i;
        }
        if (medium_dist < best_medium) {
            best_medium = medium_dist;
            color_medium = i;
        }
    }
}

void puttruecolor(int x, int y, int r, int g, int b) {
    int best = ((r & 0xe0) | ((g >> 3) & 0x1c) | ((b >> 6) & 0x03));
    putcolor_a(x, y, best);
}

void gettruecolor(int x, int y, int* r, int* g, int* b) {
    int c = getcolor(x, y) & 255;
    *r = (int)dac_channel_to_rgb(dacbox[c][0]);
    *g = (int)dac_channel_to_rgb(dacbox[c][1]);
    *b = (int)dac_channel_to_rgb(dacbox[c][2]);
}

void movecursor(int row, int col) {
    if (row != -1) {
        if (row < 0) row = 0;
        if (row >= TEXT_ROWS) row = TEXT_ROWS - 1;
        textrow = row;
    }
    if (col != -1) {
        if (col < 0) col = 0;
        if (col >= TEXT_COLS) col = TEXT_COLS - 1;
        textcol = col;
    }
}

void setattr(int row, int col, int attr, int count) {
    int first_row;
    int first_col;
    int last_row;
    int last_col;

    if (count <= 0) {
        return;
    }
    if (text_ensure_display() < 0) {
        return;
    }
    movecursor(row, col);
    first_row = textrow;
    first_col = textcol;
    last_row = textrow;
    last_col = textcol;
    while (count-- > 0) {
        s_text_attrs[textrow][textcol] = (unsigned short)attr;
        text_draw_cell(textrow, textcol);
        last_row = textrow;
        last_col = textcol;
        textcol++;
        if (textcol >= TEXT_COLS) {
            textcol = 0;
            textrow++;
            if (textrow >= TEXT_ROWS) {
                textrow = TEXT_ROWS - 1;
                break;
            }
        }
    }
    text_present_cells(first_row, first_col, last_row, last_col);
}

void putstring(int row, int col, int attr, char far* msg) {
    int first_row;
    int first_col;
    int last_row;
    int last_col;

    movecursor(row, col);
    if (!msg || text_ensure_display() < 0) {
        return;
    }

    first_row = textrow;
    first_col = textcol;
    last_row = textrow;
    last_col = textcol;
    while (*msg) {
        unsigned char ch = (unsigned char)*msg++;

        if (ch == '\n') {
            textcol = 0;
            textrow++;
        } else if (ch == '\r') {
            textcol = 0;
        } else if (ch == '\t') {
            do {
                s_text_chars[textrow][textcol] = ' ';
                s_text_attrs[textrow][textcol] = (unsigned short)attr;
                text_draw_cell(textrow, textcol);
                last_row = textrow;
                last_col = textcol;
                textcol++;
            } while ((textcol & 7) != 0 && textcol < TEXT_COLS);
        } else {
            s_text_chars[textrow][textcol] = ch;
            s_text_attrs[textrow][textcol] = (unsigned short)attr;
            text_draw_cell(textrow, textcol);
            last_row = textrow;
            last_col = textcol;
            textcol++;
        }

        if (textcol >= TEXT_COLS) {
            textcol = 0;
            textrow++;
        }
        if (textrow >= TEXT_ROWS) {
            text_scroll_range(0, TEXT_ROWS - 1);
            textrow = TEXT_ROWS - 1;
            first_row = 0;
            first_col = 0;
            last_row = TEXT_ROWS - 1;
            last_col = TEXT_COLS - 1;
        }
    }
    text_present_cells(first_row, first_col, last_row, last_col);
}

void stackscreen(void) {
    int next = screenctr + 1;

    if (next >= 0 && next <= TEXT_STACK_MAX) {
        s_text_stack_rc[next] = textrow * TEXT_COLS + textcol;
    }
    screenctr = next;
    if (screenctr > 0) {
        int slot = screenctr - 1;
        if (slot < TEXT_STACK_MAX) {
            memcpy(s_text_stack[slot].chars, s_text_chars, sizeof(s_text_chars));
            memcpy(s_text_stack[slot].attrs, s_text_attrs, sizeof(s_text_attrs));
            s_text_stack[slot].row = textrow;
            s_text_stack[slot].col = textcol;
        }
        setclear();
    } else {
        setfortext();
    }
}

void unstackscreen(void) {
    if (screenctr >= 0 && screenctr <= TEXT_STACK_MAX) {
        textrow = s_text_stack_rc[screenctr] / TEXT_COLS;
        textcol = s_text_stack_rc[screenctr] % TEXT_COLS;
    }
    screenctr--;
    if (screenctr >= 0) {
        int slot = screenctr;
        if (slot < TEXT_STACK_MAX) {
            memcpy(s_text_chars, s_text_stack[slot].chars, sizeof(s_text_chars));
            memcpy(s_text_attrs, s_text_stack[slot].attrs, sizeof(s_text_attrs));
            textrow = s_text_stack[slot].row;
            textcol = s_text_stack[slot].col;
        }
        text_redraw_all();
    } else {
        setforgraphics();
    }
    movecursor(-1, -1);
}

void discardscreen(void) {
    screenctr--;
    if (screenctr < 0) {
        s_text_visible = 0;
    }
}

void redrawscreen(void) {
    if (gfx_indexed_is_open(&s_indexed)) {
        if (s_text_visible) {
            text_redraw_all();
        } else {
            recolor_screen();
        }
    }
}

void clearbox(void) {
    unsigned char* values = (unsigned char*)boxvalues;

    for (int i = 0; i < boxcount; i++) {
        putcolor_a(boxx[i] - sxoffs, boxy[i] - syoffs, values[i]);
    }
    flush_dirty();
}

void dispbox(void) {
    unsigned char* values = (unsigned char*)boxvalues;
    int boxc = (colors - 1) & boxcolor;

    for (int i = 0; i < boxcount; i++) {
        values[i] = (unsigned char)getcolor(boxx[i] - sxoffs, boxy[i] - syoffs);
    }
    for (int i = 0; i < boxcount; i++) {
        int color = colors == 2 ? 1 - values[i] : boxc;
        putcolor_a(boxx[i] - sxoffs, boxy[i] - syoffs, color);
    }
    flush_dirty();
}

void spindac(int dir, int inc) {
    int top;
    int len;

    if (colors < 16) {
        return;
    }
    if (dir != 0 && rotate_lo < colors && rotate_lo < rotate_hi) {
        top = rotate_hi >= colors ? colors - 1 : rotate_hi;
        len = top - rotate_lo;
        while (inc-- > 0 && len > 0) {
            BYTE tmp[3];
            BYTE* base = (BYTE*)dacbox + rotate_lo * 3;

            if (dir > 0) {
                memcpy(tmp, base, sizeof(tmp));
                memmove(base, base + 3, (size_t)len * 3u);
                memcpy(base + len * 3, tmp, sizeof(tmp));
            } else {
                memcpy(tmp, base + len * 3, sizeof(tmp));
                memmove(base + 3, base, (size_t)len * 3u);
                memcpy(base, tmp, sizeof(tmp));
            }
        }
    }
    writevideopalette();
}
int resizeWindow(void) { return 0; }
void schedulealarm(int soon) { (void)soon; }
void loaddac(void) { readvideopalette(); }
void setfortext(void) { text_redraw_all(); }
void setforgraphics(void) { drain_pending_input(); flush_dirty(); recolor_screen(); s_text_visible = 0; }
void setclear(void) { text_clear((unsigned short)(BLACK * 16 + L_WHITE)); }
void home(void) { textrow = 0; textcol = 0; }
void scrollup(int top, int bot) { text_scroll_range(top, bot); }
int keycursor(int row, int col) { movecursor(row, col); return xgetkey(1); }
BYTE* findfont(int fontparm) { (void)fontparm; return NULL; }
void xpopup(char* str) { Xmessage = str; }

int xgetkey(int block) {
    int c = term_key_read(block);
    if (c == 0) {
        return 0;
    }
    if (c == 'q' || c == 'Q') {
        return ESC;
    }
    switch (c) {
        case TERM_KEY_ENTER: return ENTER;
        case TERM_KEY_ESC: return ESC;
        case TERM_KEY_UP: return UP_ARROW;
        case TERM_KEY_DOWN: return DOWN_ARROW;
        case TERM_KEY_RIGHT: return RIGHT_ARROW;
        case TERM_KEY_LEFT: return LEFT_ARROW;
        case TERM_KEY_HOME: return HOME;
        case TERM_KEY_END: return END;
        case TERM_KEY_INSERT: return INSERT;
        case TERM_KEY_DELETE: return DELETE;
        case TERM_KEY_PAGE_UP: return PAGE_UP;
        case TERM_KEY_PAGE_DOWN: return PAGE_DOWN;
        case TERM_KEY_F1: return F1;
        case TERM_KEY_F2: return F2;
        case TERM_KEY_F3: return F3;
        case TERM_KEY_F4: return F4;
        default: return c;
    }
}

char get_a_char(void) {
    return (char)term_key_read_raw(1);
}

void put_a_char(int ch) {
    char c = (char)ch;
    (void)write(STDOUT_FILENO, &c, 1u);
}
