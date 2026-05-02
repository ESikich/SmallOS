#include "user_lib.h"
#include "fcntl.h"
#include "unistd.h"

static int starts_with(const unsigned char* buf, const unsigned char* ref, unsigned int len) {
    for (unsigned int i = 0; i < len; i++) {
        if (buf[i] != ref[i]) return 0;
    }
    return 1;
}

static int bytes_equal(const unsigned char* a, const unsigned char* b, unsigned int len) {
    for (unsigned int i = 0; i < len; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static int read_exact_file(const char* path, unsigned char* buf, unsigned int len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int r = read(fd, buf, len);
    close(fd);
    return r;
}

static unsigned char large_pattern(unsigned int pos) {
    return (unsigned char)((pos * 37u + 11u) & 0xFFu);
}

static void fill_large_chunk(unsigned char* buf, unsigned int base, unsigned int len) {
    for (unsigned int i = 0; i < len; i++) {
        buf[i] = large_pattern(base + i);
    }
}

static int verify_large_chunk(const unsigned char* buf, unsigned int base, unsigned int len) {
    for (unsigned int i = 0; i < len; i++) {
        if (buf[i] != large_pattern(base + i)) return 0;
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

    if (u_file_open_read(&f, "apps/demo/hello.elf") < 0) {
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

    if (u_file_open_write(&f, "seektest.tmp") < 0) {
        u_puts("seek overwrite: FAIL\n");
        ok = 0;
    } else {
        static const unsigned char seed[] = "ABCDE";
        static const unsigned char patch[] = "xy";
        static const unsigned char expect[] = "ABxyE";
        unsigned char readback[sizeof(expect) - 1];

        if (u_file_write(&f, seed, sizeof(seed) - 1) != (int)(sizeof(seed) - 1)
            || u_file_seek(&f, 2, 0) != 2
            || u_file_write(&f, patch, sizeof(patch) - 1) != (int)(sizeof(patch) - 1)
            || u_file_close(&f) < 0
            || u_file_open_read(&f, "seektest.tmp") < 0
            || u_file_read(&f, readback, sizeof(readback)) != (int)sizeof(readback)
            || !bytes_equal(readback, expect, sizeof(readback))) {
            u_puts("seek overwrite: FAIL\n");
            ok = 0;
        } else {
            u_puts("seek overwrite: PASS\n");
        }
        u_file_close(&f);
    }

    if (u_file_open_write(&f, "seektest.tmp") < 0) {
        u_puts("truncate reopen: FAIL\n");
        ok = 0;
    } else {
        static const unsigned char one[] = "Z";
        if (u_file_write(&f, one, sizeof(one) - 1) != (int)(sizeof(one) - 1)
            || u_file_close(&f) < 0
            || u_file_stat("seektest.tmp", &size, &is_dir) < 0
            || size != 1
            || is_dir != 0) {
            u_puts("truncate reopen: FAIL\n");
            ok = 0;
        } else {
            u_puts("truncate reopen: PASS\n");
        }
    }

    (void)u_file_delete("seektest.tmp");

    {
        static const unsigned char ab[] = "AB";
        static const unsigned char cd[] = "CD";
        static const unsigned char z[] = "z";
        static const unsigned char appended[] = "ABCD";
        static const unsigned char patched[] = "AzCD";
        unsigned char readback[4];
        int fd = open("openmode.tmp", O_WRONLY | O_CREAT | O_TRUNC);

        if (fd < 0
            || write(fd, ab, sizeof(ab) - 1) != (int)(sizeof(ab) - 1)
            || close(fd) < 0
            || (fd = open("openmode.tmp", O_WRONLY | O_APPEND)) < 0
            || write(fd, cd, sizeof(cd) - 1) != (int)(sizeof(cd) - 1)
            || close(fd) < 0
            || read_exact_file("openmode.tmp", readback, sizeof(readback)) != (int)sizeof(readback)
            || !bytes_equal(readback, appended, sizeof(readback))) {
            u_puts("open append: FAIL\n");
            ok = 0;
        } else {
            u_puts("open append: PASS\n");
        }

        fd = open("openmode.tmp", O_RDWR);
        if (fd < 0
            || read(fd, readback, 2) != 2
            || !bytes_equal(readback, ab, sizeof(ab) - 1)
            || lseek(fd, 1, SEEK_SET) != 1
            || write(fd, z, sizeof(z) - 1) != (int)(sizeof(z) - 1)
            || close(fd) < 0
            || read_exact_file("openmode.tmp", readback, sizeof(readback)) != (int)sizeof(readback)
            || !bytes_equal(readback, patched, sizeof(readback))) {
            u_puts("open rdwr: FAIL\n");
            ok = 0;
        } else {
            u_puts("open rdwr: PASS\n");
        }

        (void)u_file_delete("openmode.tmp");
    }

    {
        enum {
            LARGE_SIZE = 384u * 1024u,
            LARGE_CHUNK = 4096u
        };
        unsigned char* chunk = (unsigned char*)malloc(LARGE_CHUNK);
        int fd;
        unsigned int pos;
        int large_ok = 1;

        if (!chunk) {
            u_puts("large write/readback: FAIL\n");
            ok = 0;
        } else {
            fd = open("large.tmp", O_WRONLY | O_CREAT | O_TRUNC);
            if (fd < 0) {
                large_ok = 0;
            } else {
                for (pos = 0; pos < LARGE_SIZE; pos += LARGE_CHUNK) {
                    fill_large_chunk(chunk, pos, LARGE_CHUNK);
                    if (write(fd, chunk, LARGE_CHUNK) != (int)LARGE_CHUNK) {
                        large_ok = 0;
                        break;
                    }
                }
                if (close(fd) < 0) {
                    large_ok = 0;
                }
            }

            if (large_ok && (u_file_stat("large.tmp", &size, &is_dir) < 0
                             || size != LARGE_SIZE
                             || is_dir != 0)) {
                large_ok = 0;
            }

            if (large_ok) {
                fd = open("large.tmp", O_RDONLY);
                if (fd < 0) {
                    large_ok = 0;
                } else {
                    for (pos = 0; pos < LARGE_SIZE; pos += LARGE_CHUNK) {
                        int r = read(fd, chunk, LARGE_CHUNK);
                        if (r != (int)LARGE_CHUNK
                            || !verify_large_chunk(chunk, pos, LARGE_CHUNK)) {
                            large_ok = 0;
                            break;
                        }
                    }
                    close(fd);
                }
            }

            if (large_ok) {
                u_puts("large write/readback: PASS\n");
            } else {
                u_puts("large write/readback: FAIL\n");
                ok = 0;
            }
            free(chunk);
        }
        (void)u_file_delete("large.tmp");
    }

    if (ok) {
        u_puts("fileprobe PASS\n");
    } else {
        u_puts("fileprobe FAIL\n");
    }
    sys_exit(ok ? 0 : 1);
}
