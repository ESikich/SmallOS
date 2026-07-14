#ifndef SMALLOS_GUI_DAMAGE_H
#define SMALLOS_GUI_DAMAGE_H

#include "region.h"

#define GUI_DAMAGE_CAPACITY 32

typedef struct {
    gui_rect_t rects[GUI_DAMAGE_CAPACITY];
    int count;
    int full;
} gui_damage_t;

void gui_damage_clear(gui_damage_t* damage);
void gui_damage_full(gui_damage_t* damage, int width, int height);
void gui_damage_add(gui_damage_t* damage, gui_rect_t rect,
                    int width, int height);

#endif
