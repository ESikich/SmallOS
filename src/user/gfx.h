#ifndef GFX_H
#define GFX_H

#include "user_syscall.h"

typedef struct gfx_surface {
    unsigned int width;
    unsigned int height;
    unsigned int pitch_pixels;
    unsigned int* pixels;
} gfx_surface_t;

typedef struct gfx_context {
    sys_display_info_t info;
    gfx_surface_t backbuffer;
    gfx_surface_t presentbuffer;
    int acquired;
} gfx_context_t;

int gfx_open(gfx_context_t* gfx);
void gfx_close(gfx_context_t* gfx);
int gfx_present(gfx_context_t* gfx);
int gfx_present_rect(gfx_context_t* gfx, unsigned int x, unsigned int y,
                     unsigned int w, unsigned int h);

void gfx_clear(gfx_surface_t* s, unsigned int color);
void gfx_put_pixel(gfx_surface_t* s, unsigned int x, unsigned int y, unsigned int color);
void gfx_fill_rect(gfx_surface_t* s, unsigned int x, unsigned int y,
                   unsigned int w, unsigned int h, unsigned int color);
void gfx_blit(gfx_surface_t* dst, unsigned int dx, unsigned int dy,
              const gfx_surface_t* src);

#endif /* GFX_H */
