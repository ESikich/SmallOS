#include "user_lib.h"
#include "image_bmp.h"
#include "gfx.h"

static void usage(void) {
    u_puts("usage: bmpview <file.bmp>\n");
}

static int read_file(const char* path, unsigned char** out_data, unsigned int* out_size) {
    unsigned int size = 0;
    int is_dir = 0;

    if (sys_stat(path, &size, &is_dir) < 0 || is_dir || size == 0) {
        u_puts("bmpview: cannot stat file\n");
        return 0;
    }

    unsigned char* data = (unsigned char*)malloc(size);
    if (!data) {
        u_puts("bmpview: out of memory\n");
        return 0;
    }

    int fd = sys_open(path);
    if (fd < 0) {
        free(data);
        u_puts("bmpview: cannot open file\n");
        return 0;
    }

    unsigned int pos = 0;
    while (pos < size) {
        int n = sys_fread(fd, (char*)data + pos, size - pos);
        if (n < 0) {
            sys_close(fd);
            free(data);
            u_puts("bmpview: read failed\n");
            return 0;
        }
        if (n == 0) {
            break;
        }
        pos += (unsigned int)n;
    }
    sys_close(fd);

    if (pos != size) {
        free(data);
        u_puts("bmpview: short read\n");
        return 0;
    }

    *out_data = data;
    *out_size = size;
    return 1;
}

static void fit_to_display(unsigned int src_w,
                           unsigned int src_h,
                           unsigned int max_w,
                           unsigned int max_h,
                           unsigned int* out_w,
                           unsigned int* out_h) {
    unsigned int dest_w = src_w;
    unsigned int dest_h = src_h;

    if (dest_w > max_w || dest_h > max_h) {
        if (src_w * max_h > src_h * max_w) {
            dest_w = max_w;
            dest_h = (src_h * dest_w) / src_w;
        } else {
            dest_h = max_h;
            dest_w = (src_w * dest_h) / src_h;
        }
        if (dest_w == 0) dest_w = 1;
        if (dest_h == 0) dest_h = 1;
    }

    *out_w = dest_w;
    *out_h = dest_h;
}

static void scale_row_nearest(const unsigned int* src,
                              unsigned int src_w,
                              unsigned int* dst,
                              unsigned int dst_w) {
    for (unsigned int x = 0; x < dst_w; x++) {
        unsigned int src_x = (x * src_w) / dst_w;
        dst[x] = src[src_x];
    }
}

static int render_bmp(const bmp_image_t* bmp, gfx_surface_t* dst) {
    unsigned int dest_w = 0;
    unsigned int dest_h = 0;
    unsigned int x0;
    unsigned int y0;
    unsigned int* src_row;
    unsigned int last_src_y = 0xFFFFFFFFu;

    fit_to_display(bmp->width, bmp->height, dst->width, dst->height,
                   &dest_w, &dest_h);

    x0 = (dst->width - dest_w) / 2u;
    y0 = (dst->height - dest_h) / 2u;

    src_row = (unsigned int*)malloc(bmp->width * sizeof(unsigned int));
    if (!src_row) {
        free(src_row);
        u_puts("bmpview: out of memory\n");
        return 0;
    }

    gfx_clear(dst, 0);
    for (unsigned int y = 0; y < dest_h; y++) {
        unsigned int src_y = (y * bmp->height) / dest_h;
        if (src_y != last_src_y) {
            int rc = bmp_decode_row_xrgb8888(bmp, src_y, src_row, bmp->width);
            if (rc != BMP_OK) {
                free(src_row);
                u_puts("bmpview: ");
                u_puts(bmp_error_string(rc));
                u_puts("\n");
                return 0;
            }
            last_src_y = src_y;
        }

        scale_row_nearest(src_row, bmp->width,
                          dst->pixels + (y0 + y) * dst->pitch_pixels + x0,
                          dest_w);
    }

    free(src_row);
    return 1;
}

static int view_bmp(const char* path) {
    unsigned char* data = 0;
    unsigned int size = 0;
    bmp_image_t bmp;
    gfx_context_t gfx;
    int rc;

    if (!read_file(path, &data, &size)) {
        return 1;
    }

    rc = bmp_parse(data, size, &bmp);
    if (rc != BMP_OK) {
        free(data);
        u_puts("bmpview: ");
        u_puts(bmp_error_string(rc));
        u_puts("\n");
        return 1;
    }

    rc = gfx_open(&gfx);
    if (rc == -1) {
        free(data);
        u_puts("bmpview: framebuffer display is not available\n");
        return 1;
    }
    if (rc == -4) {
        free(data);
        u_puts("bmpview: out of memory\n");
        return 1;
    }
    if (rc < 0) {
        free(data);
        u_puts("bmpview: could not acquire display\n");
        return 1;
    }

    if (!render_bmp(&bmp, &gfx.backbuffer)) {
        gfx_close(&gfx);
        free(data);
        return 1;
    }
    if (gfx_present(&gfx) < 0) {
        gfx_close(&gfx);
        free(data);
        u_puts("bmpview: present failed\n");
        return 1;
    }

    char ch;
    sys_read_raw(&ch, 1u);
    gfx_close(&gfx);

    free(data);
    return 0;
}

void _start(int argc, char** argv) {
    if (argc < 2) {
        usage();
        sys_exit(1);
    }
    sys_exit(view_bmp(argv[1]));
}
