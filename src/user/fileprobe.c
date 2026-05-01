#include "user_lib.h"

static int starts_with(const unsigned char* buf, const unsigned char* ref, unsigned int len) {
    for (unsigned int i = 0; i < len; i++) {
        if (buf[i] != ref[i]) return 0;
    }
    return 1;
}

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    u_puts("fileprobe start\n");

    u_file_t f;
    unsigned char buf[16];
    unsigned char out[] = "fileprobe tmp\n";
    uint32_t size = 0;
    int is_dir = -1;
    int ok = 1;

    if (u_file_open_read(&f, "hello.elf") < 0) {
        u_puts("open read: FAIL\n");
        ok = 0;
    } else {
        if (u_file_read(&f, buf, sizeof(buf)) != (int)sizeof(buf)) {
            u_puts("read: FAIL\n");
            ok = 0;
        } else {
            static const unsigned char elf_magic[] = { 0x7F, 'E', 'L', 'F' };
            if (!starts_with(buf, elf_magic, sizeof(elf_magic))) {
                u_puts("elf magic: FAIL\n");
                ok = 0;
            } else {
                u_puts("elf magic: PASS\n");
            }
        }
        u_file_close(&f);
    }

    if (u_file_open_write(&f, "fileprobe.tmp") < 0) {
        u_puts("open write: FAIL\n");
        ok = 0;
    } else {
        if (u_file_write(&f, out, (uint32_t)(sizeof(out) - 1)) != (int)(sizeof(out) - 1)) {
            u_puts("write: FAIL\n");
            ok = 0;
        } else {
            u_puts("write: PASS\n");
        }
        u_file_close(&f);
    }

    if (u_file_stat("fileprobe.tmp", &size, &is_dir) == 0 && size == sizeof(out) - 1 && is_dir == 0) {
        u_puts("stat tmp: PASS\n");
    } else {
        u_puts("stat tmp: FAIL\n");
        ok = 0;
    }

    if (u_file_rename("fileprobe.tmp", "fileprobe.moved") == 0) {
        u_puts("rename: PASS\n");
    } else {
        u_puts("rename: FAIL\n");
        ok = 0;
    }

    if (u_file_delete("fileprobe.moved") == 0) {
        u_puts("delete: PASS\n");
    } else {
        u_puts("delete: FAIL\n");
        ok = 0;
    }

    if (ok) {
        u_puts("fileprobe PASS\n");
    } else {
        u_puts("fileprobe FAIL\n");
    }
    sys_exit(ok ? 0 : 1);
}
