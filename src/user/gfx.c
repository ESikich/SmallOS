#include "gfx.h"
#include "user_lib.h"

int gfx_open(gfx_context_t* gfx) {
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

    if (gfx->info.width * gfx->info.height > 0xFFFFFFFFu / sizeof(unsigned int)) {
        gfx_close(gfx);
        return -3;
    }

    if (!gfx_surface_alloc(&gfx->backbuffer, gfx->info.width, gfx->info.height)) {
        gfx_close(gfx);
        return -4;
    }

    if (!gfx_surface_alloc(&gfx->presentbuffer, gfx->info.width, gfx->info.height)) {
        gfx_close(gfx);
        return -4;
    }

    gfx_clear(&gfx->backbuffer, 0);
    return 0;
}

void gfx_close(gfx_context_t* gfx) {
    if (!gfx) {
        return;
    }

    gfx_surface_free(&gfx->backbuffer);
    gfx_surface_free(&gfx->presentbuffer);

    if (gfx->acquired) {
        sys_display_release();
        gfx->acquired = 0;
    }
}

int gfx_present(gfx_context_t* gfx) {
    if (!gfx || !gfx->acquired || !gfx->backbuffer.pixels ||
        !gfx->presentbuffer.pixels) {
        return -1;
    }

    return gfx_present_rect(gfx, 0, 0,
                            gfx->backbuffer.width,
                            gfx->backbuffer.height);
}

int gfx_present_rect(gfx_context_t* gfx, unsigned int x, unsigned int y,
                     unsigned int w, unsigned int h) {
    if (!gfx || !gfx->acquired || !gfx->backbuffer.pixels ||
        !gfx->presentbuffer.pixels) {
        return -1;
    }
    if (x >= gfx->backbuffer.width || y >= gfx->backbuffer.height ||
        w == 0 || h == 0) {
        return 0;
    }
    if (w > gfx->backbuffer.width - x) {
        w = gfx->backbuffer.width - x;
    }
    if (h > gfx->backbuffer.height - y) {
        h = gfx->backbuffer.height - y;
    }

    if (w < gfx->backbuffer.pitch_pixels / 4u ||
        w * h < 32768u) {
        for (unsigned int row = 0; row < h; row++) {
            memcpy(gfx->presentbuffer.pixels + row * w,
                   gfx->backbuffer.pixels + (y + row) * gfx->backbuffer.pitch_pixels + x,
                   w * sizeof(unsigned int));
        }
        return sys_display_blit(x, y, w, h, gfx->presentbuffer.pixels);
    }

    return sys_display_blit_stride(
        x, y, w, h,
        gfx->backbuffer.pitch_pixels,
        gfx->backbuffer.pixels + y * gfx->backbuffer.pitch_pixels + x);
}

int gfx_present_surface(gfx_context_t* gfx, unsigned int x, unsigned int y,
                        const gfx_surface_t* surface) {
    unsigned int w;
    unsigned int h;

    if (!gfx || !gfx->acquired || !surface || !surface->pixels ||
        surface->width == 0 || surface->height == 0 ||
        x >= gfx->info.width || y >= gfx->info.height) {
        return -1;
    }

    w = surface->width;
    h = surface->height;
    if (w > gfx->info.width - x) {
        w = gfx->info.width - x;
    }
    if (h > gfx->info.height - y) {
        h = gfx->info.height - y;
    }
    if (w == 0 || h == 0) {
        return 0;
    }

    if (surface->pitch_pixels == surface->width) {
        return sys_display_blit(x, y, w, h, surface->pixels);
    }

    return sys_display_blit_stride(x, y, w, h,
                                   surface->pitch_pixels,
                                   surface->pixels);
}

int gfx_surface_alloc(gfx_surface_t* surface, unsigned int width,
                      unsigned int height) {
    unsigned int pixels;

    if (!surface || width == 0 || height == 0 ||
        width > 0xFFFFFFFFu / height) {
        return 0;
    }

    pixels = width * height;
    if (pixels > 0xFFFFFFFFu / sizeof(unsigned int)) {
        return 0;
    }

    surface->pixels = (unsigned int*)malloc(pixels * sizeof(unsigned int));
    if (!surface->pixels) {
        surface->width = 0;
        surface->height = 0;
        surface->pitch_pixels = 0;
        return 0;
    }

    surface->width = width;
    surface->height = height;
    surface->pitch_pixels = width;
    return 1;
}

int gfx_surface_alloc_like(gfx_surface_t* surface,
                           const gfx_surface_t* like) {
    if (!like) {
        return 0;
    }
    return gfx_surface_alloc(surface, like->width, like->height);
}

void gfx_surface_free(gfx_surface_t* surface) {
    if (!surface) {
        return;
    }
    if (surface->pixels) {
        free(surface->pixels);
    }
    surface->width = 0;
    surface->height = 0;
    surface->pitch_pixels = 0;
    surface->pixels = 0;
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

void gfx_copy_rect(gfx_surface_t* dst, unsigned int dx, unsigned int dy,
                   const gfx_surface_t* src, unsigned int sx,
                   unsigned int sy, unsigned int w, unsigned int h) {
    if (!dst || !dst->pixels || !src || !src->pixels ||
        dx >= dst->width || dy >= dst->height ||
        sx >= src->width || sy >= src->height ||
        w == 0 || h == 0) {
        return;
    }
    if (w > dst->width - dx) {
        w = dst->width - dx;
    }
    if (w > src->width - sx) {
        w = src->width - sx;
    }
    if (h > dst->height - dy) {
        h = dst->height - dy;
    }
    if (h > src->height - sy) {
        h = src->height - sy;
    }

    for (unsigned int i = 0; i < h; i++) {
        unsigned int y = (dst == src && dy > sy) ? h - 1u - i : i;
        unsigned int* dst_row = dst->pixels + (dy + y) * dst->pitch_pixels + dx;
        const unsigned int* src_row = src->pixels + (sy + y) * src->pitch_pixels + sx;
        memmove(dst_row, src_row, w * sizeof(unsigned int));
    }
}
