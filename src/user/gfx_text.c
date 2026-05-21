#include "gfx_text.h"

#include "stddef.h"

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

static const unsigned char* text_glyph(char ch) {
    size_t i;

    for (i = 0; i < sizeof(s_font) / sizeof(s_font[0]); i++) {
        if (s_font[i].ch == ch) {
            return s_font[i].rows;
        }
    }
    return s_font[0].rows;
}

unsigned int gfx_text_vga_color(int color) {
    static const unsigned int table[16] = {
        0x00000000u, 0x000000AAu, 0x0000AA00u, 0x0000AAAAu,
        0x00AA0000u, 0x00AA00AAu, 0x00AA5500u, 0x00AAAAAAu,
        0x00555555u, 0x005555FFu, 0x0055FF55u, 0x0055FFFFu,
        0x00FF5555u, 0x00FF55FFu, 0x00FFFF55u, 0x00FFFFFFu,
    };

    return table[color & 15];
}

void gfx_text_draw_cell(gfx_surface_t* s, unsigned int x, unsigned int y,
                        unsigned int cell_w, unsigned int cell_h,
                        unsigned char ch, unsigned int attr) {
    unsigned int bg = (attr >> 4) & 15u;
    unsigned int fg = attr & 15u;
    unsigned int scale_x;
    unsigned int scale_y;
    unsigned int glyph_w;
    unsigned int glyph_h;
    unsigned int glyph_x;
    unsigned int glyph_y;
    const unsigned char* glyph;

    if (!s || !s->pixels || cell_w == 0 || cell_h == 0) {
        return;
    }

    if (attr & GFX_TEXT_ATTR_INVERSE) {
        unsigned int tmp = bg;
        bg = fg;
        fg = tmp;
    }
    if (attr & GFX_TEXT_ATTR_BRIGHT) {
        fg |= 8u;
    }

    gfx_fill_rect(s, x, y, cell_w, cell_h, gfx_text_vga_color((int)bg));

    scale_x = cell_w / 6u;
    scale_y = cell_h / 9u;
    if (scale_x == 0) scale_x = 1;
    if (scale_y == 0) scale_y = 1;
    glyph_w = 5u * scale_x;
    glyph_h = 7u * scale_y;
    glyph_x = x + (cell_w > glyph_w ? (cell_w - glyph_w) / 2u : 0u);
    glyph_y = y + (cell_h > glyph_h ? (cell_h - glyph_h) / 2u : 0u);

    glyph = text_glyph((char)ch);
    for (unsigned int gy = 0; gy < 7u; gy++) {
        unsigned int bits = glyph[gy];
        for (unsigned int gx = 0; gx < 5u; gx++) {
            if (bits & (1u << (4u - gx))) {
                gfx_fill_rect(s,
                              glyph_x + gx * scale_x,
                              glyph_y + gy * scale_y,
                              scale_x,
                              scale_y,
                              gfx_text_vga_color((int)fg));
            }
        }
    }
}
