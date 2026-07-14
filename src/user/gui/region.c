#include "region.h"

gui_rect_t gui_rect_make(int x, int y, int w, int h) {
    gui_rect_t r;
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    return r;
}

int gui_rect_empty(gui_rect_t r) {
    return r.w <= 0 || r.h <= 0;
}

int gui_rect_intersects(gui_rect_t a, gui_rect_t b) {
    return !gui_rect_empty(a) && !gui_rect_empty(b) &&
           a.x < b.x + b.w && b.x < a.x + a.w &&
           a.y < b.y + b.h && b.y < a.y + a.h;
}

static int gui_rect_touches(gui_rect_t a, gui_rect_t b) {
    return !gui_rect_empty(a) && !gui_rect_empty(b) &&
           a.x <= b.x + b.w + 1 && b.x <= a.x + a.w + 1 &&
           a.y <= b.y + b.h + 1 && b.y <= a.y + a.h + 1;
}

gui_rect_t gui_rect_union(gui_rect_t a, gui_rect_t b) {
    int x0 = a.x < b.x ? a.x : b.x;
    int y0 = a.y < b.y ? a.y : b.y;
    int x1 = a.x + a.w > b.x + b.w ? a.x + a.w : b.x + b.w;
    int y1 = a.y + a.h > b.y + b.h ? a.y + a.h : b.y + b.h;
    return gui_rect_make(x0, y0, x1 - x0, y1 - y0);
}

static unsigned int gui_rect_area(gui_rect_t r) {
    if (gui_rect_empty(r)) return 0;
    return (unsigned int)r.w * (unsigned int)r.h;
}

int gui_rect_should_merge(gui_rect_t a, gui_rect_t b) {
    gui_rect_t combined;
    unsigned int separate;

    if (!gui_rect_touches(a, b)) return 0;
    combined = gui_rect_union(a, b);
    separate = gui_rect_area(a) + gui_rect_area(b);
    return gui_rect_area(combined) <= separate * 2u;
}

gui_rect_t gui_rect_clip(gui_rect_t r, int width, int height) {
    int x0 = r.x;
    int y0 = r.y;
    int x1 = r.x + r.w;
    int y1 = r.y + r.h;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > width) x1 = width;
    if (y1 > height) y1 = height;
    return gui_rect_make(x0, y0, x1 - x0, y1 - y0);
}
