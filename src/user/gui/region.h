#ifndef SMALLOS_GUI_REGION_H
#define SMALLOS_GUI_REGION_H

typedef struct {
    int x, y, w, h;
} gui_rect_t;

gui_rect_t gui_rect_make(int x, int y, int w, int h);
int gui_rect_empty(gui_rect_t r);
int gui_rect_intersects(gui_rect_t a, gui_rect_t b);
gui_rect_t gui_rect_union(gui_rect_t a, gui_rect_t b);
int gui_rect_should_merge(gui_rect_t a, gui_rect_t b);
gui_rect_t gui_rect_clip(gui_rect_t r, int width, int height);

#endif
