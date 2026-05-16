#include "diag_util.h"

void _start(int argc, char** argv) {
    sys_meminfo_t info;
    unsigned int used_frames;

    (void)argc;
    (void)argv;

    if (sys_meminfo(&info) < 0) {
        u_puts("meminfo: unavailable\n");
        sys_exit(1);
    }

    used_frames = info.pmm_total_frames - info.pmm_free_frames;
    u_puts("heap:   base ");
    diag_put_hex32(info.heap_base);
    u_puts("  top ");
    diag_put_hex32(info.heap_top);
    u_puts("  used ");
    u_put_uint((info.heap_top - info.heap_base) / 1024u);
    u_puts(" KB\n");

    u_puts("frames: ");
    u_put_uint(info.pmm_free_frames);
    u_puts(" free / ");
    u_put_uint(info.pmm_total_frames);
    u_puts(" total  (");
    u_put_uint(info.pmm_free_frames * 4u);
    u_puts(" KB / ");
    u_put_uint(info.pmm_total_frames * 4u);
    u_puts(" KB)\n");

    u_puts("used:   ");
    u_put_uint(used_frames);
    u_puts(" frames (");
    u_put_uint(used_frames * 4u);
    u_puts(" KB)\n");

    u_puts("e820:   ");
    if (info.e820_valid) {
        u_put_uint(info.e820_count);
        u_puts(" entries\n");
    } else {
        u_puts("unavailable\n");
    }

    sys_exit(0);
}
