#include "layout.h"

void gui_vlayout_begin(gui_vlayout_t* layout, gui_rect_t bounds, int gap) {
    if (!layout) return;
    layout->bounds = bounds;
    layout->cursor = bounds.y;
    layout->gap = gap < 0 ? 0 : gap;
}

gui_rect_t gui_vlayout_take(gui_vlayout_t* layout, int height) {
    gui_rect_t result;
    int bottom;
    if (!layout || height <= 0) return gui_rect_make(0, 0, 0, 0);
    bottom = layout->bounds.y + layout->bounds.h;
    if (layout->cursor >= bottom) return gui_rect_make(0, 0, 0, 0);
    if (layout->cursor + height > bottom) height = bottom - layout->cursor;
    result = gui_rect_make(layout->bounds.x, layout->cursor,
                           layout->bounds.w, height);
    layout->cursor += height + layout->gap;
    return result;
}

gui_rect_t gui_layout_inset(gui_rect_t bounds, int inset) {
    if (inset < 0) inset = 0;
    bounds.x += inset;
    bounds.y += inset;
    bounds.w -= inset * 2;
    bounds.h -= inset * 2;
    if (bounds.w < 0) bounds.w = 0;
    if (bounds.h < 0) bounds.h = 0;
    return bounds;
}

gui_rect_t gui_layout_cell(gui_rect_t row, int count, int gap, int index) {
    int width;
    if (count <= 0 || index < 0 || index >= count)
        return gui_rect_make(0, 0, 0, 0);
    if (gap < 0) gap = 0;
    width = (row.w - gap * (count - 1)) / count;
    if (width < 0) width = 0;
    return gui_rect_make(row.x + index * (width + gap), row.y,
                         width, row.h);
}
