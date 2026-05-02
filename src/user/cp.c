#include "fcntl.h"
#include "user_lib.h"

static const char* basename_of(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    return base;
}

static int copy_path(char* out, unsigned int out_size, const char* dir, const char* base) {
    unsigned int pos = 0;

    if (!out || out_size == 0u) {
        return 0;
    }

    while (dir && *dir) {
        if (pos + 1u >= out_size) return 0;
        out[pos++] = *dir++;
    }
    if (pos > 0 && out[pos - 1] != '/' && out[pos - 1] != '\\') {
        if (pos + 1u >= out_size) return 0;
        out[pos++] = '/';
    }
    while (base && *base) {
        if (pos + 1u >= out_size) return 0;
        out[pos++] = *base++;
    }
    out[pos] = '\0';
    return 1;
}

void _start(int argc, char** argv) {
    if (argc < 3) {
        u_puts("usage: cp <src> <dst>\n");
        sys_exit(1);
    }

    char dst_path[128];
    const char* dst = argv[2];
    uint32_t size = 0;
    int is_dir = 0;

    if (u_stat(argv[2], &size, &is_dir) == 0 && is_dir) {
        if (!copy_path(dst_path, sizeof(dst_path), argv[2], basename_of(argv[1]))) {
            u_puts("cp: failed\n");
            sys_exit(1);
        }
        dst = dst_path;
    }

    int in = sys_open(argv[1]);
    if (in < 0) {
        u_puts("cp: failed\n");
        sys_exit(1);
    }

    int out = sys_open_mode(dst, SYS_OPEN_MODE_WRITE | SYS_OPEN_MODE_CREATE | SYS_OPEN_MODE_TRUNC);
    if (out < 0) {
        sys_close(in);
        u_puts("cp: failed\n");
        sys_exit(1);
    }

    char buf[512];
    for (;;) {
        int n = sys_fread(in, buf, sizeof(buf));
        if (n < 0) {
            sys_close(in);
            sys_close(out);
            u_puts("cp: failed\n");
            sys_exit(1);
        }
        if (n == 0) {
            break;
        }
        if (sys_writefd(out, buf, (uint32_t)n) != n) {
            sys_close(in);
            sys_close(out);
            u_puts("cp: failed\n");
            sys_exit(1);
        }
    }

    sys_close(in);
    sys_close(out);

    u_puts("cp: ");
    u_puts(argv[1]);
    u_puts(" -> ");
    u_puts(argv[2]);
    u_putc('\n');
    sys_exit(0);
}
