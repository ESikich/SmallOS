#include "user_lib.h"

void _start(int argc, char** argv) {
    u_puts("hello from elf via int 0x80\n");
    u_puts("argc = ");
    u_put_uint((uint32_t)argc);
    u_puts("\n");

    for (int i = 0; i < argc; i++) {
        u_puts("argv[");
        u_put_uint((uint32_t)i);
        u_puts("] = \"");
        u_puts(argv[i]);
        u_puts("\"\n");
    }

    u_puts("ticks = ");
    u_put_uint(sys_get_ticks());
    u_puts("\n");

    sys_exit(0);
}