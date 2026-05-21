#ifndef GFX_TEXT_H
#define GFX_TEXT_H

#include "gfx.h"

#define GFX_TEXT_DEFAULT_CELL_W 12u
#define GFX_TEXT_DEFAULT_CELL_H 18u
#define GFX_TEXT_ATTR_INVERSE 0x8000u
#define GFX_TEXT_ATTR_BRIGHT  0x4000u

unsigned int gfx_text_vga_color(int color);
void gfx_text_draw_cell(gfx_surface_t* s, unsigned int x, unsigned int y,
                        unsigned int cell_w, unsigned int cell_h,
                        unsigned char ch, unsigned int attr);

#endif
