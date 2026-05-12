#include "gfx.h"
#include "user_lib.h"

int gfx_open(gfx_context_t* gfx) {
    unsigned int pixels;

    if (!gfx) {
        return -1;
    }

    memset(gfx, 0, sizeof(*gfx));

    if (sys_display_info(&gfx->info) < 0 ||
        gfx->info.format != SYS_DISPLAY_FORMAT_XRGB8888 ||
        gfx->info.bpp != 32u ||
        gfx->info.width == 0 ||
        gfx->info.height == 0) {
        return -1;
    }

    if (sys_display_acquire() < 0) {
        return -2;
    }
    gfx->acquired = 1;

    if (gfx->info.width > 0xFFFFFFFFu / gfx->info.height) {
        gfx_close(gfx);
        return -3;
    }

    pixels = gfx->info.width * gfx->info.height;
    if (pixels > 0xFFFFFFFFu / sizeof(unsigned int)) {
        gfx_close(gfx);
        return -3;
    }

    gfx->backbuffer.pixels = (unsigned int*)malloc(pixels * sizeof(unsigned int));
    if (!gfx->backbuffer.pixels) {
        gfx_close(gfx);
        return -4;
    }

    gfx->backbuffer.width = gfx->info.width;
    gfx->backbuffer.height = gfx->info.height;
    gfx->backbuffer.pitch_pixels = gfx->info.width;
    gfx_clear(&gfx->backbuffer, 0);
    return 0;
}

void gfx_close(gfx_context_t* gfx) {
    if (!gfx) {
        return;
    }

    if (gfx->backbuffer.pixels) {
        free(gfx->backbuffer.pixels);
        gfx->backbuffer.pixels = 0;
    }

    gfx->backbuffer.width = 0;
    gfx->backbuffer.height = 0;
    gfx->backbuffer.pitch_pixels = 0;

    if (gfx->acquired) {
        sys_display_release();
        gfx->acquired = 0;
    }
}

int gfx_present(gfx_context_t* gfx) {
    if (!gfx || !gfx->acquired || !gfx->backbuffer.pixels) {
        return -1;
    }

    return sys_display_blit(0, 0,
                            gfx->backbuffer.width,
                            gfx->backbuffer.height,
                            gfx->backbuffer.pixels);
}

void gfx_clear(gfx_surface_t* s, unsigned int color) {
    if (!s || !s->pixels) {
        return;
    }

    for (unsigned int y = 0; y < s->height; y++) {
        unsigned int* row = s->pixels + y * s->pitch_pixels;
        for (unsigned int x = 0; x < s->width; x++) {
            row[x] = color;
        }
    }
}

void gfx_put_pixel(gfx_surface_t* s, unsigned int x, unsigned int y, unsigned int color) {
    if (!s || !s->pixels || x >= s->width || y >= s->height) {
        return;
    }

    s->pixels[y * s->pitch_pixels + x] = color;
}

void gfx_fill_rect(gfx_surface_t* s, unsigned int x, unsigned int y,
                   unsigned int w, unsigned int h, unsigned int color) {
    if (!s || !s->pixels || x >= s->width || y >= s->height) {
        return;
    }

    if (w > s->width - x) {
        w = s->width - x;
    }
    if (h > s->height - y) {
        h = s->height - y;
    }

    for (unsigned int py = 0; py < h; py++) {
        unsigned int* row = s->pixels + (y + py) * s->pitch_pixels + x;
        for (unsigned int px = 0; px < w; px++) {
            row[px] = color;
        }
    }
}

void gfx_blit(gfx_surface_t* dst, unsigned int dx, unsigned int dy,
              const gfx_surface_t* src) {
    if (!dst || !dst->pixels || !src || !src->pixels) {
        return;
    }
    if (dx >= dst->width || dy >= dst->height) {
        return;
    }

    for (unsigned int y = 0; y < src->height; y++) {
        if (y >= dst->height - dy) {
            break;
        }

        for (unsigned int x = 0; x < src->width; x++) {
            if (x >= dst->width - dx) {
                break;
            }
            dst->pixels[(dy + y) * dst->pitch_pixels + dx + x] =
                src->pixels[y * src->pitch_pixels + x];
        }
    }
}
