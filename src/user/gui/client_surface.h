#ifndef SMALLOS_GUI_CLIENT_SURFACE_H
#define SMALLOS_GUI_CLIENT_SURFACE_H

#include "canvas.h"

int gui_client_surface_prepare(gfx_surface_t* desktop,
                               gui_rect_t client_screen,
                               const gui_rect_t* screen_clip,
                               gfx_surface_t* client,
                               gui_rect_t* client_clip);
void gui_client_pointer(gui_rect_t client_screen, int screen_x, int screen_y,
                        int* client_x, int* client_y);

#endif
