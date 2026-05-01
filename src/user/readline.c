#include "user_lib.h"

/*
 * readline — interactive SYS_READ test program.
 *
 * Prompts the user for their name, reads a line of input, then
 * echoes it back.  Demonstrates that SYS_READ blocks until the
 * user presses Enter, and that the kernel routes keystrokes to the
 * process input buffer (not the shell) while the process is running.
 *
 * Usage:
 *   runelf apps/tests/readline
 */
void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    char buf[128];

    u_puts("readline test\n");
    u_puts("--------------\n");

    u_puts("Enter your name: ");
    u_readline(buf, sizeof(buf));

    u_puts("Hello, ");
    u_puts(buf);
    u_puts("!\n");

    u_puts("\nType a line (max 127 chars): ");
    u_readline(buf, sizeof(buf));

    u_puts("You typed: \"");
    u_puts(buf);
    u_puts("\"\n");

    u_puts("Length: ");
    uint32_t len = 0;
    while (buf[len]) len++;
    u_put_uint(len);
    u_puts(" chars\n");

    sys_exit(0);
}
