#ifndef SMALLOS_GUI_CURSOR_H
#define SMALLOS_GUI_CURSOR_H

#include "canvas.h"

#define GUI_CURSOR_WIDTH 9
#define GUI_CURSOR_HEIGHT 12

gui_rect_t gui_cursor_rect(int x, int y);
void gui_cursor_draw(gfx_surface_t* surface, int x, int y);

#endif
