#ifndef SMALLOS_GUI_CANVAS_H
#define SMALLOS_GUI_CANVAS_H

#include "gfx.h"
#include "region.h"

void gui_canvas_set_clip(gui_rect_t r);
void gui_canvas_clear_clip(void);
int gui_canvas_has_clip(void);
const gui_rect_t* gui_canvas_clip(void);

void gui_canvas_put_pixel(gfx_surface_t* surface, int x, int y,
                          unsigned int color);
void gui_canvas_fill_rect(gfx_surface_t* surface, int x, int y, int width,
                          int height, unsigned int color);
void gui_canvas_hline(gfx_surface_t* surface, int x, int y, int width,
                      unsigned int color);
void gui_canvas_vline(gfx_surface_t* surface, int x, int y, int height,
                      unsigned int color);
void gui_canvas_rect(gfx_surface_t* surface, int x, int y, int width,
                     int height, unsigned int color);

#endif
