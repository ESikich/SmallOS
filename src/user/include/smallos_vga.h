#ifndef SMALLOS_VGA_H
#define SMALLOS_VGA_H

#include <stdint.h>
#include <string.h>

typedef struct smallos_vga_planar {
    unsigned char* planes[4];
    unsigned char* pages;
    unsigned int plane_size;
    unsigned int page_count;
    unsigned int page_bytes;
    unsigned int screen_width;
    unsigned int screen_height;
    unsigned int stride_bytes;
    int dirty;
} smallos_vga_planar_t;

static inline void smallos_vga_planar_init(smallos_vga_planar_t* vga,
                                           unsigned char* plane0,
                                           unsigned char* plane1,
                                           unsigned char* plane2,
                                           unsigned char* plane3,
                                           unsigned int plane_size,
                                           unsigned char* pages,
                                           unsigned int page_count,
                                           unsigned int page_bytes,
                                           unsigned int screen_width,
                                           unsigned int screen_height,
                                           unsigned int stride_bytes) {
    if (!vga) {
        return;
    }
    vga->planes[0] = plane0;
    vga->planes[1] = plane1;
    vga->planes[2] = plane2;
    vga->planes[3] = plane3;
    vga->plane_size = plane_size;
    vga->pages = pages;
    vga->page_count = page_count;
    vga->page_bytes = page_bytes;
    vga->screen_width = screen_width;
    vga->screen_height = screen_height;
    vga->stride_bytes = stride_bytes;
    vga->dirty = 0;
}

static inline unsigned int smallos_vga_page_from_offset(
    const smallos_vga_planar_t* vga,
    unsigned int offset) {
    if (!vga || vga->page_count == 0u || vga->page_bytes == 0u) {
        return 0u;
    }
    for (unsigned int page = vga->page_count; page > 0u; page--) {
        unsigned int index = page - 1u;
        if (offset >= index * vga->page_bytes) {
            return index;
        }
    }
    return 0u;
}

static inline unsigned int smallos_vga_page_base(
    const smallos_vga_planar_t* vga,
    unsigned int page) {
    if (!vga || page >= vga->page_count) {
        return 0u;
    }
    return page * vga->page_bytes;
}

static inline int smallos_vga_offset_to_page(const smallos_vga_planar_t* vga,
                                             unsigned int offset,
                                             unsigned int* page,
                                             unsigned int* within) {
    unsigned int candidate;
    unsigned int base;

    if (!vga || vga->page_count == 0u || vga->page_bytes == 0u ||
        offset >= vga->page_bytes * vga->page_count) {
        return 0;
    }
    candidate = smallos_vga_page_from_offset(vga, offset);
    base = smallos_vga_page_base(vga, candidate);
    if (page) {
        *page = candidate;
    }
    if (within) {
        *within = offset - base;
    }
    return 1;
}

static inline unsigned char* smallos_vga_page_pixels(
    smallos_vga_planar_t* vga,
    unsigned int page) {
    if (!vga || !vga->pages || page >= vga->page_count ||
        vga->screen_width == 0u || vga->screen_height == 0u) {
        return 0;
    }
    return vga->pages + page * vga->screen_width * vga->screen_height;
}

static inline unsigned char smallos_vga_planar_load(
    const smallos_vga_planar_t* vga,
    unsigned int offset,
    unsigned int plane) {
    if (!vga || plane >= 4u || !vga->planes[plane] ||
        offset >= vga->plane_size) {
        return 0;
    }
    return vga->planes[plane][offset];
}

static inline void smallos_vga_planar_store(smallos_vga_planar_t* vga,
                                            unsigned int offset,
                                            unsigned int plane,
                                            unsigned char color) {
    unsigned int page;
    unsigned int within;
    unsigned int xbyte;
    unsigned int y;
    unsigned char* page_pixels;

    if (!vga || plane >= 4u || !vga->planes[plane] ||
        offset >= vga->plane_size) {
        return;
    }
    vga->planes[plane][offset] = color;
    if (!smallos_vga_offset_to_page(vga, offset, &page, &within)) {
        return;
    }
    y = within / vga->stride_bytes;
    xbyte = within % vga->stride_bytes;
    if (y >= vga->screen_height || xbyte * 4u + plane >= vga->screen_width) {
        return;
    }
    page_pixels = smallos_vga_page_pixels(vga, page);
    if (!page_pixels) {
        return;
    }
    page_pixels[y * vga->screen_width + xbyte * 4u + plane] = color;
    vga->dirty = 1;
}

static inline void smallos_vga_write_planar_byte(smallos_vga_planar_t* vga,
                                                 void* base,
                                                 unsigned int byte_offset,
                                                 unsigned int plane,
                                                 unsigned char value) {
    unsigned char* ptr = (unsigned char*)base;
    uintptr_t start;
    uintptr_t end;
    uintptr_t raw;

    if (!vga || !ptr) {
        return;
    }
    if (!vga->planes[0]) {
        ptr[byte_offset] = value;
        return;
    }

    start = (uintptr_t)vga->planes[0];
    end = start + vga->plane_size;
    raw = (uintptr_t)ptr;
    if (raw < start || raw >= end) {
        ptr[byte_offset] = value;
        return;
    }
    smallos_vga_planar_store(vga, (unsigned int)(raw - start) + byte_offset,
                             plane, value);
}

static inline void smallos_vga_clear(smallos_vga_planar_t* vga,
                                     unsigned char color) {
    unsigned int page_pixels;

    if (!vga) {
        return;
    }
    for (unsigned int plane = 0; plane < 4u; plane++) {
        if (vga->planes[plane]) {
            memset(vga->planes[plane], color, vga->plane_size);
        }
    }
    page_pixels = vga->screen_width * vga->screen_height;
    for (unsigned int page = 0; page < vga->page_count; page++) {
        unsigned char* pixels = smallos_vga_page_pixels(vga, page);
        if (pixels) {
            memset(pixels, color, page_pixels);
        }
    }
    vga->dirty = 1;
}

static inline void smallos_vga_rebuild_page(smallos_vga_planar_t* vga,
                                            unsigned int page) {
    unsigned int base;
    unsigned char* pixels;

    if (!vga || page >= vga->page_count) {
        return;
    }
    pixels = smallos_vga_page_pixels(vga, page);
    if (!pixels) {
        return;
    }
    base = smallos_vga_page_base(vga, page);
    for (unsigned int y = 0; y < vga->screen_height; y++) {
        for (unsigned int xbyte = 0; xbyte < vga->stride_bytes; xbyte++) {
            unsigned int offset = base + y * vga->stride_bytes + xbyte;

            for (unsigned int plane = 0; plane < 4u; plane++) {
                if (xbyte * 4u + plane < vga->screen_width) {
                    pixels[y * vga->screen_width + xbyte * 4u + plane] =
                        smallos_vga_planar_load(vga, offset, plane);
                }
            }
        }
    }
}

static inline void smallos_vga_copy_bytes(smallos_vga_planar_t* vga,
                                          unsigned int source,
                                          unsigned int dest,
                                          unsigned int width_bytes,
                                          unsigned int height,
                                          unsigned int line_width) {
    if (!vga || line_width == 0u) {
        return;
    }
    if (width_bytes > vga->stride_bytes) {
        width_bytes = vga->stride_bytes;
    }
    if (height > vga->screen_height) {
        height = vga->screen_height;
    }

    for (unsigned int row = 0; row < height; row++) {
        unsigned int srcrow = source + row * line_width;
        unsigned int dstrow = dest + row * line_width;

        for (unsigned int col = 0; col < width_bytes; col++) {
            for (unsigned int plane = 0; plane < 4u; plane++) {
                smallos_vga_planar_store(
                    vga, dstrow + col, plane,
                    smallos_vga_planar_load(vga, srcrow + col, plane));
            }
        }
    }
}

static inline void smallos_vga_put_pixel(smallos_vga_planar_t* vga,
                                         unsigned int base,
                                         int x, int y,
                                         unsigned char color) {
    unsigned int offset;
    unsigned int plane;

    if (!vga || x < 0 || y < 0 ||
        (unsigned int)x >= vga->screen_width ||
        (unsigned int)y >= vga->screen_height) {
        return;
    }
    offset = base + (unsigned int)y * vga->stride_bytes + (unsigned int)x / 4u;
    plane = (unsigned int)x & 3u;
    smallos_vga_planar_store(vga, offset, plane, color);
}

#endif /* SMALLOS_VGA_H */
