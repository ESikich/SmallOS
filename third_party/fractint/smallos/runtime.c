#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <ctype.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>

#include "gfx.h"
#include "port.h"
#include "prototyp.h"
#include "user_lib.h"

#define SMALLOS_KEY_F2 1060
#define SMALLOS_MAX(a, b) ((a) > (b) ? (a) : (b))

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

static gfx_context_t s_gfx;
static int s_gfx_open;
static BYTE* s_pixels;
static unsigned int s_pixel_w;
static unsigned int s_pixel_h;
static unsigned int s_dot_count;
static unsigned int s_dirty_min_x;
static unsigned int s_dirty_min_y;
static unsigned int s_dirty_max_x;
static unsigned int s_dirty_max_y;
static int s_dirty_valid;
static int s_present_batch_depth;
static int s_palette_ready;

#define TEXT_COLS 80
#define TEXT_ROWS 25
#define TEXT_CELL_W 12u
#define TEXT_CELL_H 18u
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

typedef struct glyph_def {
    char ch;
    unsigned char rows[7];
} glyph_def_t;

#define G(c, r0, r1, r2, r3, r4, r5, r6) {c, {r0, r1, r2, r3, r4, r5, r6}}

static const glyph_def_t s_font[] = {
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

#undef G

static unsigned int dac_channel_to_rgb(BYTE value) {
    unsigned int v = (unsigned int)value;

    if (v <= 63u) {
        return (v << 2) | (v >> 4);
    }
    return v & 255u;
}

static unsigned int palette_color(int color) {
    unsigned int r;
    unsigned int g;
    unsigned int b;

    color &= 255;
    if (!s_palette_ready) {
        readvideopalette();
    }
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

static void recolor_screen(void) {
    unsigned int y;

    if (!s_gfx_open || !s_pixels || !s_gfx.backbuffer.pixels) {
        return;
    }
    for (y = 0; y < s_pixel_h; y++) {
        unsigned int x;
        for (x = 0; x < s_pixel_w; x++) {
            BYTE c = s_pixels[y * s_pixel_w + x];
            gfx_put_pixel(&s_gfx.backbuffer, x, y, palette_color(c));
        }
    }
    s_dirty_valid = 0;
    gfx_present(&s_gfx);
}

static void clear_graphics_pixels(BYTE color) {
    unsigned int rgb;

    if (!s_gfx_open || !s_pixels || !s_gfx.backbuffer.pixels) {
        return;
    }
    memset(s_pixels, color, (size_t)s_pixel_w * (size_t)s_pixel_h);
    rgb = palette_color(color);
    gfx_clear(&s_gfx.backbuffer, rgb);
    s_dirty_valid = 0;
    s_dot_count = 0;
    gfx_present(&s_gfx);
}

static void mark_dirty(unsigned int x, unsigned int y) {
    if (!s_dirty_valid) {
        s_dirty_min_x = x;
        s_dirty_max_x = x;
        s_dirty_min_y = y;
        s_dirty_max_y = y;
        s_dirty_valid = 1;
        return;
    }
    if (x < s_dirty_min_x) s_dirty_min_x = x;
    if (x > s_dirty_max_x) s_dirty_max_x = x;
    if (y < s_dirty_min_y) s_dirty_min_y = y;
    if (y > s_dirty_max_y) s_dirty_max_y = y;
}

static void mark_dirty_rect(unsigned int x, unsigned int y,
                            unsigned int w, unsigned int h) {
    unsigned int x1;
    unsigned int y1;

    if (w == 0 || h == 0) {
        return;
    }
    x1 = x + w - 1u;
    y1 = y + h - 1u;
    mark_dirty(x, y);
    mark_dirty(x1, y1);
}

static void flush_dirty(void) {
    if (!s_dirty_valid || !s_gfx_open) {
        return;
    }
    gfx_present_rect(&s_gfx, s_dirty_min_x, s_dirty_min_y,
                     s_dirty_max_x - s_dirty_min_x + 1u,
                     s_dirty_max_y - s_dirty_min_y + 1u);
    s_dirty_valid = 0;
}

void smallos_graphics_batch_begin(void) {
    if (s_present_batch_depth < 1024) {
        s_present_batch_depth++;
    }
}

void smallos_graphics_batch_end(void) {
    if (s_present_batch_depth > 0) {
        s_present_batch_depth--;
    }
    if (s_present_batch_depth == 0) {
        flush_dirty();
    }
}

static int open_graphics(void) {
    unsigned int pixels;
    unsigned int w;
    unsigned int h;

    if (s_gfx_open) {
        return 0;
    }
    if (gfx_open(&s_gfx) < 0) {
        return -1;
    }
    s_gfx_open = 1;
    w = s_gfx.backbuffer.width;
    h = s_gfx.backbuffer.height;
    pixels = w * h;
    s_pixels = (BYTE*)malloc(pixels);
    if (!s_pixels) {
        gfx_close(&s_gfx);
        s_gfx_open = 0;
        return -1;
    }
    memset(s_pixels, 0, pixels);
    s_dirty_valid = 0;
    s_pixel_w = w;
    s_pixel_h = h;
    videotable[0].xdots = (int)w;
    videotable[0].ydots = (int)h;
    videotable[0].colors = 256;
    gfx_clear(&s_gfx.backbuffer, 0);
    gfx_present(&s_gfx);
    return 0;
}

static void close_graphics(void) {
    if (s_pixels) {
        free(s_pixels);
        s_pixels = NULL;
    }
    s_pixel_w = 0;
    s_pixel_h = 0;
    s_dirty_valid = 0;
    if (s_gfx_open) {
        gfx_close(&s_gfx);
        s_gfx_open = 0;
    }
}

static const unsigned char* text_glyph(char ch) {
    size_t i;

    for (i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++) {
        if (s_font[i].ch == ch) {
            return s_font[i].rows;
        }
    }
    return s_font[0].rows;
}

static unsigned int text_color(int color) {
    static const unsigned int table[16] = {
        0x00000000u, 0x000000AAu, 0x0000AA00u, 0x0000AAAAu,
        0x00AA0000u, 0x00AA00AAu, 0x00AA5500u, 0x00AAAAAAu,
        0x00555555u, 0x005555FFu, 0x0055FF55u, 0x0055FFFFu,
        0x00FF5555u, 0x00FF55FFu, 0x00FFFF55u, 0x00FFFFFFu,
    };

    return table[color & 15];
}

static unsigned int text_origin_x(void) {
    unsigned int w = TEXT_COLS * TEXT_CELL_W;
    if (!s_pixel_w || s_pixel_w <= w) {
        return 0;
    }
    return (s_pixel_w - w) / 2u;
}

static unsigned int text_origin_y(void) {
    unsigned int h = TEXT_ROWS * TEXT_CELL_H;
    if (!s_pixel_h || s_pixel_h <= h) {
        return 0;
    }
    return (s_pixel_h - h) / 2u;
}

static void drain_pending_input(void) {
    char c;
    int idle_ticks = 0;

    keybuffer = 0;
    while (idle_ticks < 3) {
        struct pollfd pfd;

        pfd.fd = 0;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 0) != 1 || (pfd.revents & POLLIN) == 0) {
            idle_ticks++;
            sys_sleep(1);
            continue;
        }
        if (sys_read_raw(&c, 1u) == 1) {
            idle_ticks = 0;
        } else {
            break;
        }
    }
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
    unsigned int bg;
    unsigned int fg;
    unsigned int attr;
    const unsigned char* glyph;

    if (!s_gfx_open || !s_gfx.backbuffer.pixels ||
        row < 0 || col < 0 || row >= TEXT_ROWS || col >= TEXT_COLS) {
        return;
    }

    attr = s_text_attrs[row][col];
    bg = (attr >> 4) & 15u;
    fg = attr & 15u;
    if (attr & INVERSE) {
        unsigned int tmp = bg;
        bg = fg;
        fg = tmp;
    }
    if (attr & BRIGHT) {
        fg |= 8u;
    }

    px0 = text_origin_x() + (unsigned int)col * TEXT_CELL_W;
    py0 = text_origin_y() + (unsigned int)row * TEXT_CELL_H;
    gfx_fill_rect(&s_gfx.backbuffer, px0, py0, TEXT_CELL_W, TEXT_CELL_H,
                  text_color((int)bg));

    glyph = text_glyph((char)s_text_chars[row][col]);
    for (unsigned int gy = 0; gy < 7u; gy++) {
        unsigned int bits = glyph[gy];
        for (unsigned int gx = 0; gx < 5u; gx++) {
            if (bits & (1u << (4u - gx))) {
                unsigned int dx = px0 + 1u + gx * 2u;
                unsigned int dy = py0 + 2u + gy * 2u;
                gfx_fill_rect(&s_gfx.backbuffer, dx, dy, 2u, 2u,
                              text_color((int)fg));
            }
        }
    }
}

static void text_present_cells(int row0, int col0, int row1, int col1) {
    unsigned int x;
    unsigned int y;
    unsigned int w;
    unsigned int h;

    if (!s_gfx_open) {
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
    gfx_present_rect(&s_gfx, x, y, w, h);
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
    if (text_ensure_display() < 0) {
        return;
    }
    gfx_fill_rect(&s_gfx.backbuffer, 0, 0, s_pixel_w, s_pixel_h, 0);
    for (int row = 0; row < TEXT_ROWS; row++) {
        for (int col = 0; col < TEXT_COLS; col++) {
            text_draw_cell(row, col);
        }
    }
    gfx_present(&s_gfx);
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
        sxdots = (int)s_pixel_w;
        sydots = (int)s_pixel_h;
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
    sxdots = (int)s_pixel_w;
    sydots = (int)s_pixel_h;
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

    if (!s_pixels || x < 0 || y < 0 ||
        (unsigned int)x >= s_pixel_w || (unsigned int)y >= s_pixel_h) {
        return 0;
    }
    return s_pixels[(unsigned int)y * s_pixel_w + (unsigned int)x];
}

void putcolor_a(int xdot, int ydot, int color) {
    int x = xdot + sxoffs;
    int y = ydot + syoffs;

    if (!s_gfx_open || !s_pixels || x < 0 || y < 0 ||
        (unsigned int)x >= s_pixel_w || (unsigned int)y >= s_pixel_h) {
        return;
    }
    color &= andcolor;
    s_pixels[(unsigned int)y * s_pixel_w + (unsigned int)x] = (BYTE)color;
    gfx_put_pixel(&s_gfx.backbuffer, (unsigned int)x, (unsigned int)y,
                  palette_color(color));
    mark_dirty((unsigned int)x, (unsigned int)y);
    if (s_present_batch_depth == 0 && (++s_dot_count & 0x3ffu) == 0) {
        flush_dirty();
    }
}

void get_line(int row, int startcol, int stopcol, BYTE* pixels) {
    int x;
    int y = row + syoffs;

    if (!s_pixels || y < 0 || (unsigned int)y >= s_pixel_h) {
        return;
    }
    for (x = startcol; x <= stopcol; x++) {
        int sx = x + sxoffs;
        *pixels++ = (sx < 0 || (unsigned int)sx >= s_pixel_w)
                        ? 0
                        : s_pixels[(unsigned int)y * s_pixel_w + (unsigned int)sx];
    }
}

void put_line(int row, int startcol, int stopcol, BYTE* pixels) {
    int x;
    int y = row + syoffs;
    int first = startcol + sxoffs;
    int last = stopcol + sxoffs;

    if (!s_gfx_open || !s_pixels || y < 0 || (unsigned int)y >= s_pixel_h) {
        return;
    }
    for (x = startcol; x <= stopcol; x++) {
        int sx = x + sxoffs;
        BYTE color = *pixels++;
        if (sx < 0 || (unsigned int)sx >= s_pixel_w) {
            continue;
        }
        s_pixels[(unsigned int)y * s_pixel_w + (unsigned int)sx] = color;
        gfx_put_pixel(&s_gfx.backbuffer, (unsigned int)sx, (unsigned int)y,
                      palette_color(color));
    }
    if (first < 0) {
        first = 0;
    }
    if ((unsigned int)last >= s_pixel_w) {
        last = (int)s_pixel_w - 1;
    }
    if (last >= first) {
        mark_dirty_rect((unsigned int)first, (unsigned int)y,
                        (unsigned int)(last - first + 1), 1u);
        if (s_present_batch_depth == 0) {
            flush_dirty();
        }
    }
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
    if (s_gfx_open) {
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
int system(const char* command) { (void)command; return -1; }

static int read_raw_char(int block) {
    char c;

    if (!block) {
        struct pollfd pfd;

        pfd.fd = 0;
        pfd.events = POLLIN;
        pfd.revents = 0;
        if (poll(&pfd, 1, 0) != 1 || (pfd.revents & POLLIN) == 0) {
            return 0;
        }
    }

    while (1) {
        if (sys_read_raw(&c, 1u) == 1) {
            return (unsigned char)c;
        }
        if (!block) {
            return 0;
        }
        sys_sleep(1);
    }
}

int xgetkey(int block) {
    int c = read_raw_char(block);
    if (c == 0) {
        return 0;
    }
    if (c == 'q' || c == 'Q') {
        return ESC;
    }
    if (c == '\n') {
        return ENTER;
    }
    if (c == 0x1b) {
        int c2 = read_raw_char(0);
        if (c2 == '[') {
            int c3 = read_raw_char(0);
            switch (c3) {
                case 'A': return UP_ARROW;
                case 'B': return DOWN_ARROW;
                case 'C': return RIGHT_ARROW;
                case 'D': return LEFT_ARROW;
                case 'H': return HOME;
                case 'F': return END;
                case '2':
                    (void)read_raw_char(0);
                    return INSERT;
                case '3':
                    (void)read_raw_char(0);
                    return DELETE;
                case '5':
                    (void)read_raw_char(0);
                    return PAGE_UP;
                case '6':
                    (void)read_raw_char(0);
                    return PAGE_DOWN;
                default: return ESC;
            }
        }
        if (c2 == 'O') {
            int c3 = read_raw_char(0);
            switch (c3) {
                case 'P': return F1;
                case 'Q': return F2;
                case 'R': return F3;
                case 'S': return F4;
                default: return ESC;
            }
        }
        return ESC;
    }
    return c;
}

char get_a_char(void) {
    return (char)read_raw_char(1);
}

void put_a_char(int ch) {
    char c = (char)ch;
    sys_write(&c, 1u);
}

int abs(int x) {
    return x < 0 ? -x : x;
}

static unsigned int s_rand_state = 1;

void srand(unsigned int seed) {
    s_rand_state = seed ? seed : 1u;
}

int rand(void) {
    s_rand_state = s_rand_state * 1103515245u + 12345u;
    return (int)((s_rand_state >> 16) & 0x7fff);
}

double atof(const char* nptr) {
    return strtod(nptr, NULL);
}

long atol(const char* nptr) {
    return strtol(nptr, NULL, 10);
}

char* ctime(const time_t* timep) {
    static char buf[32];
    long t = timep ? (long)*timep : 0;
    snprintf(buf, sizeof(buf), "SmallOS time %ld\n", t);
    return buf;
}

void bcopy(const void* src, void* dst, size_t len) {
    memmove(dst, src, len);
}

void bzero(void* dst, size_t len) {
    memset(dst, 0, len);
}

int bcmp(const void* a, const void* b, size_t len) {
    return memcmp(a, b, len);
}

int strncasecmp(const char* a, const char* b, size_t n) {
    return strnicmp(a, b, n);
}

int select(int nfds, fd_set* readfds, fd_set* writefds,
           fd_set* exceptfds, struct timeval* timeout) {
    (void)nfds;
    (void)readfds;
    (void)writefds;
    (void)exceptfds;
    if (timeout) {
        unsigned int ms = (unsigned int)(timeout->tv_sec * 1000 +
                          timeout->tv_usec / 1000);
        sys_sleep(SMALLOS_MAX(1u, (ms + 54u) / 55u));
    }
    return 0;
}

int setvbuf(FILE* stream, char* buf, int mode, size_t size) {
    (void)stream;
    (void)buf;
    (void)mode;
    (void)size;
    return 0;
}

void rewind(FILE* stream) {
    fseek(stream, 0, SEEK_SET);
    clearerr(stream);
}

int putc(int c, FILE* stream) {
    return fputc(c, stream);
}

int putw(int w, FILE* stream) {
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(w & 0xff);
    bytes[1] = (unsigned char)((w >> 8) & 0xff);
    bytes[2] = (unsigned char)((w >> 16) & 0xff);
    bytes[3] = (unsigned char)((w >> 24) & 0xff);
    return fwrite(bytes, 1, 4, stream) == 4 ? 0 : EOF;
}

int getw(FILE* stream) {
    unsigned char bytes[4];
    if (fread(bytes, 1, 4, stream) != 4) {
        return EOF;
    }
    return (int)bytes[0] |
           ((int)bytes[1] << 8) |
           ((int)bytes[2] << 16) |
           ((int)bytes[3] << 24);
}

long clock(void) {
    return (long)sys_get_ticks();
}

int usleep(unsigned int usec) {
    unsigned int ticks = (usec + 54999u) / 55000u;
    if (ticks == 0) {
        ticks = 1;
    }
    sys_sleep(ticks);
    return 0;
}

static int scan_token(FILE* stream, char* out, size_t cap) {
    int c;
    size_t n = 0;

    do {
        c = fgetc(stream);
        if (c == EOF) {
            return 0;
        }
    } while (isspace(c));
    while (c != EOF && !isspace(c)) {
        if (n + 1 < cap) {
            out[n++] = (char)c;
        }
        c = fgetc(stream);
    }
    out[n] = '\0';
    return 1;
}

static int char_in_set(int c, const char* set) {
    while (*set) {
        if (c == (unsigned char)*set++) {
            return 1;
        }
    }
    return 0;
}

int vsscanf(const char* input, const char* fmt, va_list ap) {
    int assigned = 0;

    while (*fmt) {
        if (isspace((unsigned char)*fmt)) {
            while (isspace((unsigned char)*fmt)) fmt++;
            while (isspace((unsigned char)*input)) input++;
            continue;
        }
        if (*fmt != '%') {
            if (*input != *fmt) break;
            input++;
            fmt++;
            continue;
        }

        fmt++;
        int width = 0;
        int longmod = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == 'l') {
            longmod = 1;
            fmt++;
        }

        if (*fmt == 'c') {
            char* out = va_arg(ap, char*);
            int count = width ? width : 1;
            while (count-- && *input) {
                *out++ = *input++;
            }
            assigned++;
        } else if (*fmt == 'd' || *fmt == 'u' || *fmt == 'x') {
            char* end = NULL;
            int base = *fmt == 'x' ? 16 : 10;
            while (isspace((unsigned char)*input)) input++;
            if (*fmt == 'u') {
                unsigned long value = strtoul(input, &end, base);
                unsigned int* out = va_arg(ap, unsigned int*);
                if (end == input) break;
                *out = (unsigned int)value;
            } else {
                long value = strtol(input, &end, base);
                if (longmod) {
                    long* out = va_arg(ap, long*);
                    *out = value;
                } else {
                    int* out = va_arg(ap, int*);
                    *out = (int)value;
                }
                if (end == input) break;
            }
            input = end;
            assigned++;
        } else if (*fmt == 'f') {
            char* end = NULL;
            double value;
            while (isspace((unsigned char)*input)) input++;
            value = strtod(input, &end);
            if (end == input) break;
            if (longmod) {
                double* out = va_arg(ap, double*);
                *out = value;
            } else {
                float* out = va_arg(ap, float*);
                *out = (float)value;
            }
            input = end;
            assigned++;
        } else if (*fmt == 's') {
            char* out = va_arg(ap, char*);
            int count = 0;
            while (isspace((unsigned char)*input)) input++;
            while (*input && !isspace((unsigned char)*input) &&
                   (!width || count < width)) {
                *out++ = *input++;
                count++;
            }
            *out = '\0';
            if (count == 0) break;
            assigned++;
        } else if (*fmt == '[') {
            char* out = va_arg(ap, char*);
            char set[64];
            int neg = 0;
            int si = 0;
            int count = 0;
            fmt++;
            if (*fmt == '^') {
                neg = 1;
                fmt++;
            }
            while (*fmt && *fmt != ']' && si + 1 < (int)sizeof(set)) {
                set[si++] = *fmt++;
            }
            set[si] = '\0';
            while (*input && (!width || count < width)) {
                int in = char_in_set((unsigned char)*input, set);
                if ((in && neg) || (!in && !neg)) break;
                *out++ = *input++;
                count++;
            }
            *out = '\0';
            if (count == 0) break;
            assigned++;
        }

        if (*fmt) {
            fmt++;
        }
    }
    return assigned;
}

int sscanf(const char* input, const char* fmt, ...) {
    va_list ap;
    int rc;
    va_start(ap, fmt);
    rc = vsscanf(input, fmt, ap);
    va_end(ap);
    return rc;
}

int fscanf(FILE* stream, const char* fmt, ...) {
    va_list ap;
    int assigned = 0;

    va_start(ap, fmt);
    while (*fmt) {
        char tok[128];
        if (*fmt++ != '%') {
            continue;
        }
        if (*fmt == 'd') {
            int* out = va_arg(ap, int*);
            if (!scan_token(stream, tok, sizeof(tok))) break;
            *out = atoi(tok);
            assigned++;
        } else if (*fmt == 'f') {
            float* out = va_arg(ap, float*);
            if (!scan_token(stream, tok, sizeof(tok))) break;
            *out = (float)atof(tok);
            assigned++;
        } else if (*fmt == 's') {
            char* out = va_arg(ap, char*);
            if (!scan_token(stream, out, 128)) break;
            assigned++;
        }
        if (*fmt) {
            fmt++;
        }
    }
    va_end(ap);
    return assigned ? assigned : EOF;
}

#undef isupper
#undef islower
int isupper(int c) { return c >= 'A' && c <= 'Z'; }
int islower(int c) { return c >= 'a' && c <= 'z'; }

double fabs(double x) {
    return x < 0.0 ? -x : x;
}

double floor(double x) {
    long i = (long)x;
    if ((double)i > x) {
        i--;
    }
    return (double)i;
}

double ceil(double x) {
    long i = (long)x;
    if ((double)i < x) {
        i++;
    }
    return (double)i;
}

double fmod(double x, double y) {
    long q;
    if (y == 0.0) {
        return 0.0;
    }
    q = (long)(x / y);
    return x - (double)q * y;
}

double sqrt(double x) {
    double g;
    int i;
    if (x <= 0.0) {
        return 0.0;
    }
    g = x > 1.0 ? x : 1.0;
    for (i = 0; i < 24; i++) {
        g = 0.5 * (g + x / g);
    }
    return g;
}

static double reduce_angle(double x) {
    const double two_pi = 6.28318530717958647692;
    const double pi = 3.14159265358979323846;
    x = fmod(x, two_pi);
    if (x > pi) x -= two_pi;
    if (x < -pi) x += two_pi;
    return x;
}

double sin(double x) {
    double x2;
    x = reduce_angle(x);
    x2 = x * x;
    return x * (1.0 - x2 / 6.0 + (x2 * x2) / 120.0 -
                (x2 * x2 * x2) / 5040.0);
}

double cos(double x) {
    double x2;
    x = reduce_angle(x);
    x2 = x * x;
    return 1.0 - x2 / 2.0 + (x2 * x2) / 24.0 -
           (x2 * x2 * x2) / 720.0;
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) {
        return x < 0.0 ? -HUGE_VAL : HUGE_VAL;
    }
    return sin(x) / c;
}

double atan(double x) {
    int neg = x < 0.0;
    double r;
    if (neg) x = -x;
    if (x > 1.0) {
        r = 1.57079632679489661923 - atan(1.0 / x);
    } else {
        r = x / (1.0 + 0.28 * x * x);
    }
    return neg ? -r : r;
}

double atan2(double y, double x) {
    if (x > 0.0) return atan(y / x);
    if (x < 0.0 && y >= 0.0) return atan(y / x) + 3.14159265358979323846;
    if (x < 0.0 && y < 0.0) return atan(y / x) - 3.14159265358979323846;
    if (y > 0.0) return 1.57079632679489661923;
    if (y < 0.0) return -1.57079632679489661923;
    return 0.0;
}

double asin(double x) {
    if (x >= 1.0) return 1.57079632679489661923;
    if (x <= -1.0) return -1.57079632679489661923;
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    return 1.57079632679489661923 - asin(x);
}

double exp(double x) {
    const double ln2 = 0.69314718055994530942;
    int n = 0;
    double term;
    double sum;
    int i;

    if (x > 60.0) return HUGE_VAL;
    if (x < -60.0) return 0.0;
    while (x > ln2) { x -= ln2; n++; }
    while (x < -ln2) { x += ln2; n--; }
    term = 1.0;
    sum = 1.0;
    for (i = 1; i <= 18; i++) {
        term *= x / (double)i;
        sum += term;
    }
    while (n > 0) { sum *= 2.0; n--; }
    while (n < 0) { sum *= 0.5; n++; }
    return sum;
}

double frexp(double x, int* exp_out) {
    int e = 0;
    double ax;
    if (x == 0.0) {
        *exp_out = 0;
        return 0.0;
    }
    ax = fabs(x);
    while (ax >= 1.0) { ax *= 0.5; e++; }
    while (ax < 0.5) { ax *= 2.0; e--; }
    *exp_out = e;
    return x < 0.0 ? -ax : ax;
}

double log(double x) {
    int e;
    double m;
    double z;
    double z2;
    double term;
    double sum;
    int i;

    if (x <= 0.0) {
        return -HUGE_VAL;
    }
    m = frexp(x, &e) * 2.0;
    e--;
    z = (m - 1.0) / (m + 1.0);
    z2 = z * z;
    term = z;
    sum = 0.0;
    for (i = 1; i < 30; i += 2) {
        sum += term / (double)i;
        term *= z2;
    }
    return 2.0 * sum + (double)e * 0.69314718055994530942;
}

double log10(double x) {
    return log(x) / 2.30258509299404568402;
}

double pow(double x, double y) {
    if (x <= 0.0) {
        return 0.0;
    }
    return exp(y * log(x));
}

double sinh(double x) {
    double e = exp(x);
    double ie = exp(-x);
    return 0.5 * (e - ie);
}

double cosh(double x) {
    double e = exp(x);
    double ie = exp(-x);
    return 0.5 * (e + ie);
}

long double sinhl(long double x) {
    return (long double)sinh((double)x);
}

long double coshl(long double x) {
    return (long double)cosh((double)x);
}

int isnan(double x) {
    return x != x;
}

int isinf(double x) {
    return !isnan(x) && isnan(x - x);
}

int islessequal(double x, double y) {
    return x <= y;
}
