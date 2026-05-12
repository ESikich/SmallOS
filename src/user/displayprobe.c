#include "user_lib.h"

void _start(int argc, char** argv) {
    sys_display_info_t info;
    unsigned int row[64];
    unsigned int w;

    (void)argc;
    (void)argv;

    u_puts("displayprobe start\n");

    if (sys_display_info(&info) < 0 ||
        info.format != SYS_DISPLAY_FORMAT_XRGB8888 ||
        info.bpp != 32u) {
        u_puts("displayprobe SKIP no framebuffer\n");
        sys_exit(0);
    }

    u_puts("displayprobe info width=");
    u_put_uint(info.width);
    u_puts(" height=");
    u_put_uint(info.height);
    u_puts(" pitch=");
    u_put_uint(info.pitch);
    u_puts(" bpp=");
    u_put_uint(info.bpp);
    u_puts(" format=");
    u_put_uint(info.format);
    u_putc('\n');

    if (sys_display_fill(0, 0, 1, 1, 0x00FF0000u) >= 0) {
        u_puts("displayprobe FAIL draw without acquire\n");
        sys_exit(1);
    }

    if (sys_display_acquire() < 0) {
        u_puts("displayprobe FAIL acquire\n");
        sys_exit(1);
    }

    w = info.width < 64u ? info.width : 64u;
    for (unsigned int i = 0; i < w; i++) {
        row[i] = (i & 1u) ? 0x0000AAFFu : 0x00FFCC00u;
    }

    if (sys_display_fill(0, 0, info.width, info.height, 0x00002020u) < 0 ||
        sys_display_fill(8, 8, w, 24u, 0x0000AA44u) < 0 ||
        sys_display_blit(8, 40, w, 1u, row) < 0) {
        sys_display_release();
        u_puts("displayprobe FAIL draw\n");
        sys_exit(1);
    }

    sys_sleep(10);
    sys_display_release();
    u_puts("displayprobe PASS\n");
    sys_exit(0);
}
