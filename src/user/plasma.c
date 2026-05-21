#include "user_lib.h"
#include "gfx.h"
#include "term_keys.h"

#define PLASMA_FRAMES 240u

static unsigned int color_from_value(unsigned int v) {
    unsigned int r = (v * 3u) & 0xFFu;
    unsigned int g = (v * 5u + 80u) & 0xFFu;
    unsigned int b = (v * 7u + 160u) & 0xFFu;

    return (r << 16) | (g << 8) | b;
}

static void draw_plasma(gfx_surface_t* s, unsigned int frame) {
    unsigned int t = frame * 3u;

    for (unsigned int y = 0; y < s->height; y++) {
        unsigned int* row = s->pixels + y * s->pitch_pixels;
        unsigned int yy = y + t;
        for (unsigned int x = 0; x < s->width; x++) {
            unsigned int xx = x + t;
            unsigned int v = ((xx ^ yy) +
                              ((x * y + frame * 97u) >> 7) +
                              ((x + frame * 5u) & 63u) +
                              ((y + frame * 2u) & 127u)) & 0xFFu;
            row[x] = color_from_value(v);
        }
    }
}

void _start(int argc, char** argv) {
    gfx_context_t gfx;
    int rc;

    (void)argc;
    (void)argv;

    u_puts("plasma: starting\n");
    rc = gfx_open(&gfx);
    if (rc == -1) {
        u_puts("plasma: framebuffer display is not available\n");
        sys_exit(0);
    }
    if (rc < 0) {
        u_puts("plasma: could not open display\n");
        sys_exit(1);
    }

    for (unsigned int frame = 0; frame < PLASMA_FRAMES; frame++) {
        if (term_key_read(0) != TERM_KEY_NONE) {
            break;
        }

        draw_plasma(&gfx.backbuffer, frame);
        if (gfx_present(&gfx) < 0) {
            gfx_close(&gfx);
            u_puts("plasma: present failed\n");
            sys_exit(1);
        }
        sys_sleep(1);
    }

    gfx_close(&gfx);
    u_puts("plasma: done\n");
    sys_exit(0);
}
