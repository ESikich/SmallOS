#ifndef GFX_INDEXED_H
#define GFX_INDEXED_H

#include "gfx.h"

typedef struct gfx_indexed_context {
    gfx_context_t gfx;
    unsigned char* pixels;
    unsigned int palette[256];
    unsigned int width;
    unsigned int height;
    unsigned int dirty_min_x;
    unsigned int dirty_min_y;
    unsigned int dirty_max_x;
    unsigned int dirty_max_y;
    unsigned int dot_count;
    int dirty_valid;
    int open;
    int batch_depth;
} gfx_indexed_context_t;

int gfx_indexed_open(gfx_indexed_context_t* ctx);
void gfx_indexed_close(gfx_indexed_context_t* ctx);
int gfx_indexed_is_open(const gfx_indexed_context_t* ctx);
unsigned int gfx_indexed_width(const gfx_indexed_context_t* ctx);
unsigned int gfx_indexed_height(const gfx_indexed_context_t* ctx);
gfx_surface_t* gfx_indexed_surface(gfx_indexed_context_t* ctx);

void gfx_indexed_set_palette(gfx_indexed_context_t* ctx, unsigned int index,
                             unsigned int color);
void gfx_indexed_recolor(gfx_indexed_context_t* ctx);
void gfx_indexed_clear(gfx_indexed_context_t* ctx, unsigned char color);

unsigned char gfx_indexed_get_pixel(const gfx_indexed_context_t* ctx,
                                    int x, int y);
void gfx_indexed_put_pixel(gfx_indexed_context_t* ctx, int x, int y,
                           unsigned char color);
void gfx_indexed_get_line(const gfx_indexed_context_t* ctx, int y,
                          int x0, int x1, unsigned char* pixels);
void gfx_indexed_put_line(gfx_indexed_context_t* ctx, int y,
                          int x0, int x1, const unsigned char* pixels);

void gfx_indexed_present(gfx_indexed_context_t* ctx);
void gfx_indexed_present_rect(gfx_indexed_context_t* ctx,
                              unsigned int x, unsigned int y,
                              unsigned int w, unsigned int h);
void gfx_indexed_flush(gfx_indexed_context_t* ctx);
void gfx_indexed_batch_begin(gfx_indexed_context_t* ctx);
void gfx_indexed_batch_end(gfx_indexed_context_t* ctx);

#endif /* GFX_INDEXED_H */
