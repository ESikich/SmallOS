#include "gfx.h"
#include "user_lib.h"
#include <stdint.h>

static void gfx_write_fence(gfx_context_t* gfx) {
    __asm__ volatile("lock; addl $0, %0"
                     : "+m"(gfx->fence_word)
                     :
                     : "memory", "cc");
}

int gfx_open(gfx_context_t* gfx) {
    int rc = gfx_open_display(gfx);

    if (rc < 0) {
        return rc;
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

int gfx_open_display(gfx_context_t* gfx) {
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

    return 0;
}

void gfx_close(gfx_context_t* gfx) {
    if (!gfx) {
        return;
    }

    gfx->mapped = 0;
    memset(&gfx->map, 0, sizeof(gfx->map));
    gfx_surface_free(&gfx->backbuffer);
    gfx_surface_free(&gfx->presentbuffer);
    gfx_surface_free(&gfx->scratch);

    if (gfx->acquired) {
        sys_display_release();
        gfx->acquired = 0;
    }
}

int gfx_map(gfx_context_t* gfx) {
    sys_display_map_info_t map;

    if (!gfx || !gfx->acquired) {
        return -1;
    }
    if (gfx->mapped) {
        return 0;
    }
    memset(&map, 0, sizeof(map));
    if (sys_display_map(&map) < 0 ||
        map.format != SYS_DISPLAY_FORMAT_XRGB8888 ||
        map.bpp != 32u ||
        map.width != gfx->info.width ||
        map.height != gfx->info.height ||
        map.pitch != gfx->info.pitch ||
        (map.pitch & 3u) != 0u ||
        map.page_count < 2u ||
        map.draw_page >= map.page_count) {
        gfx->mapped = 0;
        memset(&gfx->map, 0, sizeof(gfx->map));
        return -1;
    }

    gfx->map = map;
    gfx->mapped = 1;
    return 0;
}

int gfx_is_mapped(const gfx_context_t* gfx) {
    return gfx && gfx->acquired && gfx->mapped;
}

int gfx_mapped_surface(gfx_context_t* gfx, gfx_surface_t* surface) {
    unsigned int page_offset;
    unsigned int row_bytes;

    if (!gfx_is_mapped(gfx) || !surface ||
        gfx->map.draw_page >= gfx->map.page_count) {
        return -1;
    }
    if (gfx->map.width == 0 || gfx->map.height == 0 ||
        gfx->map.page_bytes == 0 ||
        (gfx->map.pitch & 3u) != 0u) {
        return -1;
    }
    if (gfx->map.height - 1u > 0xFFFFFFFFu / gfx->map.pitch ||
        gfx->map.width > 0xFFFFFFFFu / sizeof(unsigned int) ||
        gfx->map.draw_page > 0xFFFFFFFFu / gfx->map.page_bytes) {
        return -1;
    }
    page_offset = gfx->map.draw_page * gfx->map.page_bytes;
    if (page_offset > 0xFFFFFFFFu - gfx->map.base) {
        return -1;
    }
    row_bytes = gfx->map.width * sizeof(unsigned int);
    if (gfx->map.pitch < row_bytes) {
        return -1;
    }
    row_bytes += (gfx->map.height - 1u) * gfx->map.pitch;
    if (row_bytes < gfx->map.width * sizeof(unsigned int)) {
        return -1;
    }
    if (row_bytes > gfx->map.page_bytes) {
        return -1;
    }

    surface->width = gfx->map.width;
    surface->height = gfx->map.height;
    surface->pitch_pixels = gfx->map.pitch / sizeof(unsigned int);
    surface->pixels = (unsigned int*)(uintptr_t)(gfx->map.base + page_offset);
    return 0;
}

int gfx_present_mapped(gfx_context_t* gfx) {
    sys_display_present_page_t req;

    if (!gfx_is_mapped(gfx) || gfx->map.draw_page >= gfx->map.page_count) {
        return -1;
    }

    gfx_write_fence(gfx);
    req.page = gfx->map.draw_page;
    req.next_page = req.page;
    if (sys_display_present_page(&req) < 0 ||
        req.next_page >= gfx->map.page_count) {
        gfx->mapped = 0;
        memset(&gfx->map, 0, sizeof(gfx->map));
        return -1;
    }
    gfx->map.draw_page = req.next_page;
    return 0;
}

int gfx_display_fill(gfx_context_t* gfx, unsigned int x, unsigned int y,
                     unsigned int w, unsigned int h, unsigned int color) {
    if (!gfx || !gfx->acquired) {
        return -1;
    }
    return sys_display_fill(x, y, w, h, color);
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

static int gfx_surface_resize(gfx_surface_t* surface, unsigned int width,
                              unsigned int height) {
    if (!surface || width == 0 || height == 0) {
        return 0;
    }
    if (surface->pixels &&
        surface->width == width &&
        surface->height == height &&
        surface->pitch_pixels == width) {
        return 1;
    }
    gfx_surface_free(surface);
    return gfx_surface_alloc(surface, width, height);
}

int gfx_present_indexed(gfx_context_t* gfx, unsigned int x, unsigned int y,
                        const gfx_indexed_surface_t* surface,
                        unsigned int scale) {
    unsigned int present_w;
    unsigned int present_h;
    gfx_surface_t mapped;

    if (!gfx || !gfx->acquired || !surface || !surface->pixels ||
        !surface->palette || surface->width == 0 || surface->height == 0 ||
        surface->pitch_pixels < surface->width || scale == 0u ||
        surface->width > 0xFFFFFFFFu / scale ||
        surface->height > 0xFFFFFFFFu / scale) {
        return -1;
    }

    present_w = surface->width * scale;
    present_h = surface->height * scale;
    if (x > gfx->info.width || y > gfx->info.height ||
        present_w > gfx->info.width - x ||
        present_h > gfx->info.height - y) {
        return -1;
    }

    if (gfx_mapped_surface(gfx, &mapped) == 0 &&
        gfx_blit_indexed_scaled(&mapped, x, y, surface, scale) == 0 &&
        gfx_present_mapped(gfx) == 0) {
        return 0;
    }

    if (!gfx_surface_resize(&gfx->scratch, present_w, present_h)) {
        return -1;
    }
    if (gfx_blit_indexed_scaled(&gfx->scratch, 0, 0, surface, scale) < 0) {
        return -1;
    }
    return gfx_present_surface(gfx, x, y, &gfx->scratch);
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

int gfx_blit_indexed_scaled(gfx_surface_t* dst, unsigned int dx,
                            unsigned int dy,
                            const gfx_indexed_surface_t* src,
                            unsigned int scale) {
    unsigned int scaled_w;

    if (!dst || !dst->pixels || !src || !src->pixels || !src->palette ||
        src->width == 0 || src->height == 0 ||
        src->pitch_pixels < src->width || scale == 0u ||
        src->width > 0xFFFFFFFFu / scale ||
        src->height > 0xFFFFFFFFu / scale) {
        return -1;
    }

    scaled_w = src->width * scale;
    if (dx > dst->width || dy > dst->height ||
        scaled_w > dst->width - dx ||
        src->height * scale > dst->height - dy) {
        return -1;
    }

    for (unsigned int src_y = 0; src_y < src->height; src_y++) {
        const unsigned char* src_row = src->pixels + src_y * src->pitch_pixels;
        unsigned int* dst_row =
            dst->pixels + (dy + src_y * scale) * dst->pitch_pixels + dx;

        for (unsigned int src_x = 0; src_x < src->width; src_x++) {
            unsigned int color = src->palette[src_row[src_x]];

            for (unsigned int sx = 0; sx < scale; sx++) {
                dst_row[src_x * scale + sx] = color;
            }
        }
        for (unsigned int sy = 1; sy < scale; sy++) {
            memcpy(dst_row + sy * dst->pitch_pixels,
                   dst_row,
                   scaled_w * sizeof(*dst_row));
        }
    }

    return 0;
}
