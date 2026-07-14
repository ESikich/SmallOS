#include "canvas.h"

static gui_rect_t canvas_clip_rect;
static int canvas_clip_enabled;

void gui_canvas_set_clip(gui_rect_t r) {
    canvas_clip_rect = r;
    canvas_clip_enabled = 1;
}

void gui_canvas_clear_clip(void) {
    canvas_clip_enabled = 0;
}

int gui_canvas_has_clip(void) {
    return canvas_clip_enabled;
}

const gui_rect_t* gui_canvas_clip(void) {
    return &canvas_clip_rect;
}

void gui_canvas_put_pixel(gfx_surface_t* s, int x, int y,
                          unsigned int color) {
    if (!s || !s->pixels || x < 0 || y < 0 ||
        x >= (int)s->width || y >= (int)s->height) return;
    if (canvas_clip_enabled &&
        (x < canvas_clip_rect.x || x >= canvas_clip_rect.x + canvas_clip_rect.w ||
         y < canvas_clip_rect.y || y >= canvas_clip_rect.y + canvas_clip_rect.h)) {
        return;
    }
    s->pixels[(unsigned int)y * s->pitch_pixels + (unsigned int)x] = color;
}

void gui_canvas_fill_rect(gfx_surface_t* s, int x, int y, int w, int h,
                          unsigned int color) {
    int x0 = x;
    int y0 = y;
    int x1 = x + w;
    int y1 = y + h;

    if (!s || !s->pixels || w <= 0 || h <= 0) return;
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (int)s->width) x1 = (int)s->width;
    if (y1 > (int)s->height) y1 = (int)s->height;
    if (canvas_clip_enabled) {
        if (x0 < canvas_clip_rect.x) x0 = canvas_clip_rect.x;
        if (y0 < canvas_clip_rect.y) y0 = canvas_clip_rect.y;
        if (x1 > canvas_clip_rect.x + canvas_clip_rect.w)
            x1 = canvas_clip_rect.x + canvas_clip_rect.w;
        if (y1 > canvas_clip_rect.y + canvas_clip_rect.h)
            y1 = canvas_clip_rect.y + canvas_clip_rect.h;
    }
    if (x1 <= x0 || y1 <= y0) return;

    for (int py = y0; py < y1; py++) {
        unsigned int* row = s->pixels +
            (unsigned int)py * s->pitch_pixels + (unsigned int)x0;
        for (int px = x0; px < x1; px++) *row++ = color;
    }
}

void gui_canvas_hline(gfx_surface_t* s, int x, int y, int w,
                      unsigned int color) {
    gui_canvas_fill_rect(s, x, y, w, 1, color);
}

void gui_canvas_vline(gfx_surface_t* s, int x, int y, int h,
                      unsigned int color) {
    gui_canvas_fill_rect(s, x, y, 1, h, color);
}

void gui_canvas_rect(gfx_surface_t* s, int x, int y, int w, int h,
                     unsigned int color) {
    if (w <= 0 || h <= 0) return;
    gui_canvas_hline(s, x, y, w, color);
    gui_canvas_hline(s, x, y + h - 1, w, color);
    gui_canvas_vline(s, x, y, h, color);
    gui_canvas_vline(s, x + w - 1, y, h, color);
}
