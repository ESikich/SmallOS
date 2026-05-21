#include "fcntl.h"
#include "stdio.h"
#include "stdlib.h"
#include "sys/stat.h"
#include "unistd.h"

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
        fputs("usage: cp <src> <dst>\n", stderr);
        exit(1);
    }

    char dst_path[128];
    const char* dst = argv[2];
    struct stat st;

    if (stat(argv[2], &st) == 0 && S_ISDIR(st.st_mode)) {
        if (!copy_path(dst_path, sizeof(dst_path), argv[2], basename_of(argv[1]))) {
            fputs("cp: failed\n", stderr);
            exit(1);
        }
        dst = dst_path;
    }

    int in = open(argv[1], O_RDONLY);
    if (in < 0) {
        fputs("cp: failed\n", stderr);
        exit(1);
    }

    int out = open(dst, O_WRONLY | O_CREAT | O_TRUNC);
    if (out < 0) {
        close(in);
        fputs("cp: failed\n", stderr);
        exit(1);
    }

    char buf[512];
    for (;;) {
        int n = read(in, buf, sizeof(buf));
        if (n < 0) {
            close(in);
            close(out);
            fputs("cp: failed\n", stderr);
            exit(1);
        }
        if (n == 0) {
            break;
        }
        int off = 0;
        while (off < n) {
            int written = write(out, buf + off, (unsigned int)(n - off));
            if (written <= 0) {
                close(in);
                close(out);
                fputs("cp: failed\n", stderr);
                exit(1);
            }
            off += written;
        }
    }

    close(in);
    close(out);

    printf("cp: %s -> %s\n", argv[1], argv[2]);
    exit(0);
}
