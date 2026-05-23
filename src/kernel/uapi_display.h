#ifndef UAPI_DISPLAY_H
#define UAPI_DISPLAY_H

#define SYS_DISPLAY_FORMAT_XRGB8888 1u
#define SYS_DISPLAY_MAP_BASE 0xB0000000u

typedef struct sys_display_info {
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int bpp;
    unsigned int format;
} sys_display_info_t;

typedef struct sys_display_fill_rect {
    unsigned int x;
    unsigned int y;
    unsigned int w;
    unsigned int h;
    unsigned int color;
} sys_display_fill_rect_t;

typedef struct sys_display_blit_rect {
    unsigned int x;
    unsigned int y;
    unsigned int w;
    unsigned int h;
    const unsigned int* pixels;
} sys_display_blit_rect_t;

typedef struct sys_display_blit_stride_rect {
    unsigned int x;
    unsigned int y;
    unsigned int w;
    unsigned int h;
    unsigned int pitch_pixels;
    const unsigned int* pixels;
} sys_display_blit_stride_rect_t;

typedef struct sys_display_map_info {
    unsigned int base;
    unsigned int width;
    unsigned int height;
    unsigned int pitch;
    unsigned int bpp;
    unsigned int format;
    unsigned int page_bytes;
    unsigned int page_count;
    unsigned int draw_page;
} sys_display_map_info_t;

typedef struct sys_display_present_page {
    unsigned int page;
    unsigned int next_page;
} sys_display_present_page_t;

#endif /* UAPI_DISPLAY_H */
