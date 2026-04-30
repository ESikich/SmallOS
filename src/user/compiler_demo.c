#include "user_lib.h"

/*
 * compiler_demo — proof that user space can emit a file artifact.
 *
 * This is the first building block for a future in-OS compiler: it
 * writes a compiler-style output file to the FAT16 root directory and
 * reads it back to verify persistence.
 */

static int bytes_equal(const char* a, const char* b, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    static const char artifact[] =
        ";; SmallOS compiler demo output\n"
        "MOV AX, 1\n"
        "ADD AX, 2\n"
        "INT 0x80\n";
    static const char file_name[] = "compiler.out";
    static const char nested_file_name[] = "apps/demo/compiler.out";

    u_puts("compiler_demo start\n");

    if (sys_writefile(file_name, artifact, str_len(artifact)) < 0) {
        u_puts("writefile failed\n");
        sys_exit(1);
    }
    u_puts("writefile: ok\n");

    int fd = sys_open(file_name);
    if (fd < 0) {
        u_puts("open failed\n");
        sys_exit(1);
    }

    char buf[sizeof(artifact)];
    int n = sys_fread(fd, buf, (uint32_t)sizeof(buf));
    if (n < 0 || (unsigned int)n != str_len(artifact)) {
        u_puts("readback failed\n");
        sys_close(fd);
        sys_exit(1);
    }

    if (!bytes_equal(buf, artifact, (unsigned int)n)) {
        u_puts("readback mismatch\n");
        sys_close(fd);
        sys_exit(1);
    }
    u_puts("readback: ok\n");

    if (sys_close(fd) < 0) {
        u_puts("close failed\n");
        sys_exit(1);
    }

    if (sys_writefile_path(nested_file_name, artifact, str_len(artifact)) < 0) {
        u_puts("writefile_path failed\n");
        sys_exit(1);
    }
    u_puts("writefile_path: ok\n");

    fd = sys_open(nested_file_name);
    if (fd < 0) {
        u_puts("nested open failed\n");
        sys_exit(1);
    }

    n = sys_fread(fd, buf, (uint32_t)sizeof(buf));
    if (n < 0 || (unsigned int)n != str_len(artifact)) {
        u_puts("nested readback failed\n");
        sys_close(fd);
        sys_exit(1);
    }

    if (!bytes_equal(buf, artifact, (unsigned int)n)) {
        u_puts("nested readback mismatch\n");
        sys_close(fd);
        sys_exit(1);
    }
    u_puts("nested readback: ok\n");

    if (sys_close(fd) < 0) {
        u_puts("nested close failed\n");
        sys_exit(1);
    }

    u_puts("compiler_demo PASS\n");
    sys_exit(0);
}
