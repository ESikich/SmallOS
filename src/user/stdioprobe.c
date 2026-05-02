#include "stdio.h"
#include "string.h"
#include "fcntl.h"
#include "unistd.h"

static int bytes_equal(const char* a, const char* b, unsigned int len) {
    for (unsigned int i = 0; i < len; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static int read_exact_file(const char* path, char* buf, unsigned int len) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    int r = read(fd, buf, len);
    close(fd);
    return r;
}

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    int ok = 1;
    char buf[16];

    puts("stdioprobe start");

    FILE* in = fopen("apps/demo/hello.elf", "r");
    if (!in) {
        puts("stdio eof: FAIL");
        sys_exit(1);
    }
    while (fgetc(in) != EOF) {
    }
    if (!feof(in) || ferror(in)) {
        puts("stdio eof: FAIL");
        ok = 0;
    } else {
        puts("stdio eof: PASS");
    }
    clearerr(in);
    if (feof(in) || ferror(in)) {
        puts("stdio clearerr: FAIL");
        ok = 0;
    } else {
        puts("stdio clearerr: PASS");
    }
    fclose(in);

    FILE* out = fopen("stdioprobe.tmp", "w");
    static const char msg[] = "stdio ok\n";
    if (!out
        || fwrite(msg, 1, sizeof(msg) - 1u, out) != sizeof(msg) - 1u
        || fflush(out) != 0
        || read_exact_file("stdioprobe.tmp", buf, sizeof(msg) - 1u) != (int)(sizeof(msg) - 1u)
        || !bytes_equal(buf, msg, sizeof(msg) - 1u)
        || fclose(out) != 0) {
        puts("stdio write+fflush: FAIL");
        ok = 0;
    } else {
        puts("stdio write+fflush: PASS");
    }
    unlink("stdioprobe.tmp");

    in = fopen("apps/demo/hello.elf", "r");
    if (!in || fwrite("x", 1, 1, in) != 0 || !ferror(in)) {
        puts("stdio write failure: FAIL");
        ok = 0;
    } else {
        puts("stdio write failure: PASS");
    }
    if (in) fclose(in);

    out = fopen("stdioprobe.tmp", "w");
    if (!out || fread(buf, 1, 1, out) != 0 || !ferror(out)) {
        puts("stdio bad read op: FAIL");
        ok = 0;
    } else {
        puts("stdio bad read op: PASS");
    }
    if (out) fclose(out);
    unlink("stdioprobe.tmp");

    FILE* bad = fdopen(99, "w");
    if (!bad || fflush(bad) == 0 || !ferror(bad)) {
        puts("stdio bad fd flush: FAIL");
        ok = 0;
    } else {
        puts("stdio bad fd flush: PASS");
    }
    if (bad) fclose(bad);

    puts(ok ? "stdioprobe PASS" : "stdioprobe FAIL");
    sys_exit(ok ? 0 : 1);
}
