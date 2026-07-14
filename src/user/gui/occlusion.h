#ifndef SMALLOS_GUI_OCCLUSION_H
#define SMALLOS_GUI_OCCLUSION_H

#include "region.h"

#define GUI_VISIBLE_REGION_CAPACITY 64

/* Subtract opaque rectangles in order. Returns -1 if capacity is exhausted. */
int gui_visible_regions(gui_rect_t source,
                        const gui_rect_t* opaque,
                        int opaque_count,
                        gui_rect_t* output,
                        int output_capacity);

#endif
