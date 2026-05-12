#include "gfx.h"
#include "user_lib.h"
#include "poll.h"

#define MANDEL_SCALE 4096
#define MANDEL_VIEW_W ((7 * MANDEL_SCALE) / 2)
#define MANDEL_CENTER_X (-(MANDEL_SCALE / 2))
#define MANDEL_CENTER_Y 0
#define MANDEL_MAX_ITER 64u
#define MANDEL_BLOCK 2u
#define MANDEL_MIN_VIEW_W 8
#define MANDEL_MAX_VIEW_W (16 * MANDEL_SCALE)
#define MANDEL_CURSOR_W 11u
#define MANDEL_CURSOR_H 17u
#define MANDEL_CURSOR_FILL 0x00FFFFFFu
#define MANDEL_CURSOR_SHADOW 0x00000000u

typedef enum mandel_key {
    MANDEL_KEY_NONE = 0,
    MANDEL_KEY_QUIT,
    MANDEL_KEY_LEFT,
    MANDEL_KEY_RIGHT,
    MANDEL_KEY_UP,
    MANDEL_KEY_DOWN,
    MANDEL_KEY_ZOOM_IN,
    MANDEL_KEY_ZOOM_OUT,
    MANDEL_KEY_RESET
} mandel_key_t;

typedef struct mandel_view {
    int center_x;
    int center_y;
    int view_w;
} mandel_view_t;

typedef struct mandel_cursor {
    unsigned int x;
    unsigned int y;
} mandel_cursor_t;

static int input_available(void) {
    struct pollfd pfd;

    pfd.fd = 0;
    pfd.events = POLLIN;
    pfd.revents = 0;
    return sys_poll(&pfd, 1u, 0) == 1 && (pfd.revents & POLLIN);
}

static mandel_key_t read_key(void) {
    char c = 0;

    if (!input_available()) {
        return MANDEL_KEY_NONE;
    }
    if (sys_read_raw(&c, 1u) != 1) {
        return MANDEL_KEY_NONE;
    }

    if (c == '+') return MANDEL_KEY_ZOOM_IN;
    if (c == '-' || c == '_') return MANDEL_KEY_ZOOM_OUT;
    if (c == 'r' || c == 'R') return MANDEL_KEY_RESET;
    if (c == 'q' || c == 'Q') return MANDEL_KEY_QUIT;

    if ((unsigned char)c != 27) {
        return MANDEL_KEY_NONE;
    }

    if (!input_available()) {
        return MANDEL_KEY_QUIT;
    }

    if (sys_read_raw(&c, 1u) != 1 || c != '[') {
        return MANDEL_KEY_QUIT;
    }
    if (sys_read_raw(&c, 1u) != 1) {
        return MANDEL_KEY_NONE;
    }

    if (c == 'A') return MANDEL_KEY_UP;
    if (c == 'B') return MANDEL_KEY_DOWN;
    if (c == 'C') return MANDEL_KEY_RIGHT;
    if (c == 'D') return MANDEL_KEY_LEFT;
    return MANDEL_KEY_NONE;
}

static unsigned int mandel_color(unsigned int iter) {
    unsigned int r;
    unsigned int g;
    unsigned int b;

    if (iter >= MANDEL_MAX_ITER) {
        return 0x00000000u;
    }

    r = (iter * 9u) & 0xFFu;
    g = (iter * 5u + 40u) & 0xFFu;
    b = (iter * 13u + 120u) & 0xFFu;
    return (r << 16) | (g << 8) | b;
}

static unsigned int mandel_iter(int cr, int ci) {
    int zr = 0;
    int zi = 0;

    for (unsigned int iter = 0; iter < MANDEL_MAX_ITER; iter++) {
        int zr2 = (zr * zr) / MANDEL_SCALE;
        int zi2 = (zi * zi) / MANDEL_SCALE;
        int zri;

        if (zr2 + zi2 > 4 * MANDEL_SCALE) {
            return iter;
        }

        zri = (zr * zi) / MANDEL_SCALE;
        zi = 2 * zri + ci;
        zr = zr2 - zi2 + cr;
    }

    return MANDEL_MAX_ITER;
}

static void fill_block(gfx_surface_t* s, unsigned int x, unsigned int y,
                       unsigned int color) {
    unsigned int max_y = y + MANDEL_BLOCK;
    unsigned int max_x = x + MANDEL_BLOCK;

    if (max_y > s->height) {
        max_y = s->height;
    }
    if (max_x > s->width) {
        max_x = s->width;
    }

    for (unsigned int py = y; py < max_y; py++) {
        unsigned int* row = s->pixels + py * s->pitch_pixels;
        for (unsigned int px = x; px < max_x; px++) {
            row[px] = color;
        }
    }
}

static void draw_mandel(gfx_surface_t* s, const mandel_view_t* view) {
    int view_h = (int)(((unsigned int)view->view_w * s->height) / s->width);
    int left = view->center_x - view->view_w / 2;
    int top = view->center_y - view_h / 2;

    for (unsigned int y = 0; y < s->height; y += MANDEL_BLOCK) {
        int ci = top + (int)((y * (unsigned int)view_h) / s->height);
        for (unsigned int x = 0; x < s->width; x += MANDEL_BLOCK) {
            int cr = left + (int)((x * (unsigned int)view->view_w) / s->width);
            unsigned int iter = mandel_iter(cr, ci);
            fill_block(s, x, y, mandel_color(iter));
        }
    }
}

static void draw_cursor_pixel(gfx_surface_t* s, int x, int y, unsigned int color) {
    if (x < 0 || y < 0) {
        return;
    }
    gfx_put_pixel(s, (unsigned int)x, (unsigned int)y, color);
}

static void draw_cursor(gfx_surface_t* s, const mandel_cursor_t* cursor) {
    static const char* shape[MANDEL_CURSOR_H] = {
        "X          ",
        "XX         ",
        "XWX        ",
        "XWWX       ",
        "XWWWX      ",
        "XWWWWX     ",
        "XWWWWWX    ",
        "XWWWWWWX   ",
        "XWWWWWWWX  ",
        "XWWWWX     ",
        "XWWXWX     ",
        "XWX XWX    ",
        "XX  XWX    ",
        "X    XWX   ",
        "     XWX   ",
        "      XWX  ",
        "      XXX  "
    };
    int left = (int)cursor->x;
    int top = (int)cursor->y;

    for (unsigned int y = 0; y < MANDEL_CURSOR_H; y++) {
        for (unsigned int x = 0; x < MANDEL_CURSOR_W; x++) {
            char pixel = shape[y][x];
            if (pixel == 'W') {
                draw_cursor_pixel(s, left + (int)x, top + (int)y,
                                  MANDEL_CURSOR_FILL);
            } else if (pixel == 'X') {
                draw_cursor_pixel(s, left + (int)x, top + (int)y,
                                  MANDEL_CURSOR_SHADOW);
            }
        }
    }
}

static void reset_view(mandel_view_t* view) {
    view->center_x = MANDEL_CENTER_X;
    view->center_y = MANDEL_CENTER_Y;
    view->view_w = MANDEL_VIEW_W;
}

static void reset_cursor(mandel_cursor_t* cursor, const gfx_surface_t* s) {
    cursor->x = s->width / 2u;
    cursor->y = s->height / 2u;
}

static int apply_mouse(mandel_cursor_t* cursor, const gfx_surface_t* s,
                       const sys_mouse_state_t* mouse) {
    int x;
    int y;

    if (mouse->dx == 0 && mouse->dy == 0) {
        return 0;
    }

    x = (int)cursor->x + mouse->dx;
    y = (int)cursor->y + mouse->dy;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= (int)s->width) x = (int)s->width - 1;
    if (y >= (int)s->height) y = (int)s->height - 1;

    if ((unsigned int)x == cursor->x && (unsigned int)y == cursor->y) {
        return 0;
    }

    cursor->x = (unsigned int)x;
    cursor->y = (unsigned int)y;
    return 1;
}

static int apply_key(mandel_view_t* view, mandel_key_t key) {
    int pan = view->view_w / 8;

    if (pan < 1) {
        pan = 1;
    }

    if (key == MANDEL_KEY_LEFT) {
        view->center_x -= pan;
    } else if (key == MANDEL_KEY_RIGHT) {
        view->center_x += pan;
    } else if (key == MANDEL_KEY_UP) {
        view->center_y -= pan;
    } else if (key == MANDEL_KEY_DOWN) {
        view->center_y += pan;
    } else if (key == MANDEL_KEY_ZOOM_IN) {
        if (view->view_w > MANDEL_MIN_VIEW_W) {
            view->view_w = (view->view_w * 3) / 4;
            if (view->view_w < MANDEL_MIN_VIEW_W) {
                view->view_w = MANDEL_MIN_VIEW_W;
            }
        }
    } else if (key == MANDEL_KEY_ZOOM_OUT) {
        if (view->view_w < MANDEL_MAX_VIEW_W) {
            view->view_w = (view->view_w * 4) / 3;
            if (view->view_w > MANDEL_MAX_VIEW_W) {
                view->view_w = MANDEL_MAX_VIEW_W;
            }
        }
    } else if (key == MANDEL_KEY_RESET) {
        reset_view(view);
    } else {
        return 0;
    }

    return 1;
}

void _start(int argc, char** argv) {
    gfx_context_t gfx;
    mandel_view_t view;
    mandel_cursor_t cursor;
    int rc;

    (void)argc;
    (void)argv;

    u_puts("mandel: arrows pan, +/- zoom, r reset, q quit\n");
    rc = gfx_open(&gfx);
    if (rc == -1) {
        u_puts("mandel: framebuffer display is not available\n");
        sys_exit(0);
    }
    if (rc < 0) {
        u_puts("mandel: could not open display\n");
        sys_exit(1);
    }

    reset_view(&view);
    reset_cursor(&cursor, &gfx.backbuffer);
    {
        sys_mouse_state_t mouse;
        (void)sys_mouse_read(&mouse);
    }

    int dirty = 1;
    for (;;) {
        mandel_key_t key;
        sys_mouse_state_t mouse;

        if (dirty) {
            dirty = 0;
            draw_mandel(&gfx.backbuffer, &view);
            draw_cursor(&gfx.backbuffer, &cursor);
            if (gfx_present(&gfx) < 0) {
                gfx_close(&gfx);
                u_puts("mandel: present failed\n");
                sys_exit(1);
            }
        }

        key = read_key();
        if (key == MANDEL_KEY_QUIT) {
            break;
        }
        if (apply_key(&view, key)) {
            dirty = 1;
        }
        if (sys_mouse_read(&mouse) == 0 &&
            apply_mouse(&cursor, &gfx.backbuffer, &mouse)) {
            dirty = 1;
        }
        if (!dirty) {
            sys_sleep(1);
        }
    }

    gfx_close(&gfx);
    sys_exit(0);
}
