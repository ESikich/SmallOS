#include "damage.h"

void gui_damage_clear(gui_damage_t* damage) {
    if (!damage) return;
    damage->count = 0;
    damage->full = 0;
}

void gui_damage_full(gui_damage_t* damage, int width, int height) {
    if (!damage) return;
    damage->full = 1;
    damage->count = 1;
    damage->rects[0] = gui_rect_make(0, 0, width, height);
}

void gui_damage_add(gui_damage_t* damage, gui_rect_t rect,
                    int width, int height) {
    if (!damage || damage->full) return;
    rect = gui_rect_clip(rect, width, height);
    if (gui_rect_empty(rect)) return;
    for (int i = 0; i < damage->count; i++) {
        if (gui_rect_should_merge(damage->rects[i], rect)) {
            damage->rects[i] = gui_rect_clip(
                gui_rect_union(damage->rects[i], rect), width, height);
            return;
        }
    }
    if (damage->count >= GUI_DAMAGE_CAPACITY) {
        gui_damage_full(damage, width, height);
        return;
    }
    damage->rects[damage->count++] = rect;
}
