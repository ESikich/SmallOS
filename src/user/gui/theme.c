#include "theme.h"

#include "canvas.h"

const gui_widget_theme_t gui_retro_widget_theme = {
    COL_BTN_BG, 0x00D8D8D8u, 0x00A8A8A8u, COL_FRAME,
    COL_TEXT, 0x00808080u, COL_TITLE_BG
};

const gui_builtin_style_t gui_retro_builtin_style = {
    COL_WIN_BG, COL_FRAME, COL_TEXT, COL_SUBTEXT
};

typedef struct {
    char ch;
    unsigned char rows[7];
} glyph_t;

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
static int g_font_ready;

static const unsigned char* glyph_for(char ch) {
    unsigned char uch = (unsigned char)ch;
    if (!g_font_ready) {
        for (unsigned int i = 0; i < FONT_COUNT; i++) {
            unsigned char glyph = (unsigned char)FONT[i].ch;
            if (glyph < 128u) g_font_ascii[glyph] = FONT[i].rows;
        }
        g_font_ready = 1;
    }
    return uch < 128u ? g_font_ascii[uch] : 0;
}

static void draw_char(gfx_surface_t* surface, int x, int y, char ch,
                      unsigned int color) {
    const unsigned char* glyph = glyph_for(ch);
    if (!glyph) {
        gui_canvas_fill_rect(surface, x + 1, y + 6, 2, 1, color);
        return;
    }
    for (unsigned int row = 0; row < 7; row++) {
        for (unsigned int col = 0; col < 5; col++) {
            if (glyph[row] & (1u << (4u - col)))
                gui_canvas_put_pixel(surface, x + (int)col, y + (int)row, color);
        }
    }
}

void gui_theme_draw_text(gfx_surface_t* surface, int x, int y,
                         const char* text, unsigned int color) {
    int x_limit;
    if (!surface || !text || y + 7 <= 0 || y >= (int)surface->height) return;
    x_limit = (int)surface->width;
    if (gui_canvas_has_clip()) {
        const gui_rect_t* clip = gui_canvas_clip();
        if (y + 7 <= clip->y || y >= clip->y + clip->h) return;
        x_limit = clip->x + clip->w;
        while (*text && x + 5 < clip->x) { x += 6; text++; }
    } else {
        while (*text && x + 5 < 0) { x += 6; text++; }
    }
    while (*text && x < x_limit) {
        draw_char(surface, x, y, *text++, color);
        x += 6;
    }
}

unsigned int gui_theme_text_width(const char* text) {
    unsigned int length = 0;
    if (!text) return 0;
    while (*text++) length++;
    return length ? length * 6u - 1u : 0u;
}

void gui_theme_draw_fixed_text(gfx_surface_t* surface, int x, int y,
                               const char* text, int max_chars,
                               unsigned int color) {
    int end = max_chars;
    int first = 0;
    int x_limit;
    if (!surface || !text || max_chars <= 0 ||
        y + 7 <= 0 || y >= (int)surface->height) return;
    x_limit = (int)surface->width;
    if (gui_canvas_has_clip()) {
        const gui_rect_t* clip = gui_canvas_clip();
        if (y + 7 <= clip->y || y >= clip->y + clip->h) return;
        x_limit = clip->x + clip->w;
        while (first < max_chars && x + first * 6 + 5 < clip->x) first++;
    } else {
        while (first < max_chars && x + first * 6 + 5 < 0) first++;
    }
    while (end > 0 && text[end - 1] == ' ') end--;
    for (int i = first; i < end && text[i] && x + i * 6 < x_limit; i++)
        draw_char(surface, x + i * 6, y, text[i], color);
}
