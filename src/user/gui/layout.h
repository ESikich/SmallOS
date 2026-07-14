#ifndef SMALLOS_GUI_LAYOUT_H
#define SMALLOS_GUI_LAYOUT_H

#include "region.h"

typedef struct {
    gui_rect_t bounds;
    int cursor;
    int gap;
} gui_vlayout_t;

void gui_vlayout_begin(gui_vlayout_t* layout, gui_rect_t bounds, int gap);
gui_rect_t gui_vlayout_take(gui_vlayout_t* layout, int height);
gui_rect_t gui_layout_inset(gui_rect_t bounds, int inset);
gui_rect_t gui_layout_cell(gui_rect_t row, int count, int gap, int index);

#endif
