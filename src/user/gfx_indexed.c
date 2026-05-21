#include "gfx_indexed.h"

#include <stdlib.h>
#include <string.h>

static void mark_dirty(gfx_indexed_context_t* ctx, unsigned int x,
                       unsigned int y) {
    if (!ctx->dirty_valid) {
        ctx->dirty_min_x = x;
        ctx->dirty_max_x = x;
        ctx->dirty_min_y = y;
        ctx->dirty_max_y = y;
        ctx->dirty_valid = 1;
        return;
    }
    if (x < ctx->dirty_min_x) ctx->dirty_min_x = x;
    if (x > ctx->dirty_max_x) ctx->dirty_max_x = x;
    if (y < ctx->dirty_min_y) ctx->dirty_min_y = y;
    if (y > ctx->dirty_max_y) ctx->dirty_max_y = y;
}

static void mark_dirty_rect(gfx_indexed_context_t* ctx, unsigned int x,
                            unsigned int y, unsigned int w,
                            unsigned int h) {
    unsigned int x1;
    unsigned int y1;

    if (w == 0 || h == 0) {
        return;
    }
    x1 = x + w - 1u;
    y1 = y + h - 1u;
    mark_dirty(ctx, x, y);
    mark_dirty(ctx, x1, y1);
}

int gfx_indexed_open(gfx_indexed_context_t* ctx) {
    unsigned int pixels;

    if (!ctx) {
        return -1;
    }
    if (ctx->open) {
        return 0;
    }

    memset(ctx, 0, sizeof(*ctx));
    if (gfx_open(&ctx->gfx) < 0) {
        return -1;
    }

    ctx->width = ctx->gfx.backbuffer.width;
    ctx->height = ctx->gfx.backbuffer.height;
    if (ctx->width == 0 || ctx->height == 0 ||
        ctx->width > 0xFFFFFFFFu / ctx->height) {
        gfx_close(&ctx->gfx);
        return -1;
    }

    pixels = ctx->width * ctx->height;
    ctx->pixels = (unsigned char*)malloc(pixels);
    if (!ctx->pixels) {
        gfx_close(&ctx->gfx);
        memset(ctx, 0, sizeof(*ctx));
        return -1;
    }

    memset(ctx->pixels, 0, pixels);
    gfx_clear(&ctx->gfx.backbuffer, ctx->palette[0]);
    ctx->open = 1;
    gfx_present(&ctx->gfx);
    return 0;
}

void gfx_indexed_close(gfx_indexed_context_t* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->pixels) {
        free(ctx->pixels);
        ctx->pixels = NULL;
    }
    gfx_close(&ctx->gfx);
    memset(ctx, 0, sizeof(*ctx));
}

int gfx_indexed_is_open(const gfx_indexed_context_t* ctx) {
    return ctx && ctx->open && ctx->pixels && ctx->gfx.backbuffer.pixels;
}

unsigned int gfx_indexed_width(const gfx_indexed_context_t* ctx) {
    return gfx_indexed_is_open(ctx) ? ctx->width : 0;
}

unsigned int gfx_indexed_height(const gfx_indexed_context_t* ctx) {
    return gfx_indexed_is_open(ctx) ? ctx->height : 0;
}

gfx_surface_t* gfx_indexed_surface(gfx_indexed_context_t* ctx) {
    if (!gfx_indexed_is_open(ctx)) {
        return NULL;
    }
    return &ctx->gfx.backbuffer;
}

void gfx_indexed_set_palette(gfx_indexed_context_t* ctx, unsigned int index,
                             unsigned int color) {
    if (!ctx || index >= 256u) {
        return;
    }
    ctx->palette[index] = color;
}

void gfx_indexed_recolor(gfx_indexed_context_t* ctx) {
    unsigned int y;

    if (!gfx_indexed_is_open(ctx)) {
        return;
    }
    for (y = 0; y < ctx->height; y++) {
        unsigned int x;
        for (x = 0; x < ctx->width; x++) {
            unsigned char c = ctx->pixels[y * ctx->width + x];
            gfx_put_pixel(&ctx->gfx.backbuffer, x, y, ctx->palette[c]);
        }
    }
    ctx->dirty_valid = 0;
    gfx_present(&ctx->gfx);
}

void gfx_indexed_clear(gfx_indexed_context_t* ctx, unsigned char color) {
    if (!gfx_indexed_is_open(ctx)) {
        return;
    }
    memset(ctx->pixels, color, (size_t)ctx->width * (size_t)ctx->height);
    gfx_clear(&ctx->gfx.backbuffer, ctx->palette[color]);
    ctx->dirty_valid = 0;
    ctx->dot_count = 0;
    gfx_present(&ctx->gfx);
}

unsigned char gfx_indexed_get_pixel(const gfx_indexed_context_t* ctx,
                                    int x, int y) {
    if (!gfx_indexed_is_open(ctx) || x < 0 || y < 0 ||
        (unsigned int)x >= ctx->width || (unsigned int)y >= ctx->height) {
        return 0;
    }
    return ctx->pixels[(unsigned int)y * ctx->width + (unsigned int)x];
}

void gfx_indexed_put_pixel(gfx_indexed_context_t* ctx, int x, int y,
                           unsigned char color) {
    if (!gfx_indexed_is_open(ctx) || x < 0 || y < 0 ||
        (unsigned int)x >= ctx->width || (unsigned int)y >= ctx->height) {
        return;
    }
    ctx->pixels[(unsigned int)y * ctx->width + (unsigned int)x] = color;
    gfx_put_pixel(&ctx->gfx.backbuffer, (unsigned int)x, (unsigned int)y,
                  ctx->palette[color]);
    mark_dirty(ctx, (unsigned int)x, (unsigned int)y);
    if (ctx->batch_depth == 0 && (++ctx->dot_count & 0x3ffu) == 0) {
        gfx_indexed_flush(ctx);
    }
}

void gfx_indexed_get_line(const gfx_indexed_context_t* ctx, int y,
                          int x0, int x1, unsigned char* pixels) {
    int x;

    if (!pixels || !gfx_indexed_is_open(ctx) || y < 0 ||
        (unsigned int)y >= ctx->height || x1 < x0) {
        return;
    }
    for (x = x0; x <= x1; x++) {
        *pixels++ = gfx_indexed_get_pixel(ctx, x, y);
    }
}

void gfx_indexed_put_line(gfx_indexed_context_t* ctx, int y,
                          int x0, int x1, const unsigned char* pixels) {
    int x;
    int first = -1;
    int last = -1;

    if (!pixels || !gfx_indexed_is_open(ctx) || y < 0 ||
        (unsigned int)y >= ctx->height || x1 < x0) {
        return;
    }

    for (x = x0; x <= x1; x++) {
        unsigned char color = *pixels++;
        if (x < 0 || (unsigned int)x >= ctx->width) {
            continue;
        }
        ctx->pixels[(unsigned int)y * ctx->width + (unsigned int)x] = color;
        gfx_put_pixel(&ctx->gfx.backbuffer, (unsigned int)x, (unsigned int)y,
                      ctx->palette[color]);
        if (first < 0) {
            first = x;
        }
        last = x;
    }

    if (first >= 0 && last >= first) {
        mark_dirty_rect(ctx, (unsigned int)first, (unsigned int)y,
                        (unsigned int)(last - first + 1), 1u);
        if (ctx->batch_depth == 0) {
            gfx_indexed_flush(ctx);
        }
    }
}

void gfx_indexed_present(gfx_indexed_context_t* ctx) {
    if (!gfx_indexed_is_open(ctx)) {
        return;
    }
    ctx->dirty_valid = 0;
    gfx_present(&ctx->gfx);
}

void gfx_indexed_present_rect(gfx_indexed_context_t* ctx,
                              unsigned int x, unsigned int y,
                              unsigned int w, unsigned int h) {
    if (!gfx_indexed_is_open(ctx)) {
        return;
    }
    gfx_present_rect(&ctx->gfx, x, y, w, h);
}

void gfx_indexed_flush(gfx_indexed_context_t* ctx) {
    if (!gfx_indexed_is_open(ctx) || !ctx->dirty_valid) {
        return;
    }
    gfx_present_rect(&ctx->gfx, ctx->dirty_min_x, ctx->dirty_min_y,
                     ctx->dirty_max_x - ctx->dirty_min_x + 1u,
                     ctx->dirty_max_y - ctx->dirty_min_y + 1u);
    ctx->dirty_valid = 0;
}

void gfx_indexed_batch_begin(gfx_indexed_context_t* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->batch_depth < 1024) {
        ctx->batch_depth++;
    }
}

void gfx_indexed_batch_end(gfx_indexed_context_t* ctx) {
    if (!ctx) {
        return;
    }
    if (ctx->batch_depth > 0) {
        ctx->batch_depth--;
    }
    if (ctx->batch_depth == 0) {
        gfx_indexed_flush(ctx);
    }
}
