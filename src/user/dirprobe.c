#include "dirent.h"
#include "stdio.h"
#include "string.h"

static int dir_contains(const char* path, const char* name) {
    DIR* d = opendir(path);
    if (!d) return 0;

    struct dirent* ent;
    while ((ent = readdir(d)) != 0) {
        unsigned int len = strlen(ent->d_name);
        if (strcasecmp(ent->d_name, name) == 0
            || (len > 0 && ent->d_name[len - 1u] == '/'
                && strlen(name) == len - 1u
                && strnicmp(ent->d_name, name, len - 1u) == 0)) {
            closedir(d);
            return 1;
        }
    }

    closedir(d);
    return 0;
}

void _start(int argc, char** argv) {
    (void)argc;
    (void)argv;

    int ok = 1;

    puts("dirprobe start");

    if (dir_contains("/", "apps")) {
        puts("dir root: PASS");
    } else {
        puts("dir root: FAIL");
        ok = 0;
    }

    if (dir_contains("apps/tests", "cwdprobe.elf")) {
        puts("dir nested: PASS");
    } else {
        puts("dir nested: FAIL");
        ok = 0;
    }

    DIR* d = opendir("apps/demo");
    if (!d) {
        puts("dir eof: FAIL");
        ok = 0;
    } else {
        while (readdir(d) != 0) {
        }
        if (readdir(d) == 0) {
            puts("dir eof: PASS");
        } else {
            puts("dir eof: FAIL");
            ok = 0;
        }
        closedir(d);
    }

    if (!opendir("apps/demo/hello.elf")) {
        puts("dir invalid file: PASS");
    } else {
        puts("dir invalid file: FAIL");
        ok = 0;
    }

    if (!opendir("apps/nope")) {
        puts("dir missing: PASS");
    } else {
        puts("dir missing: FAIL");
        ok = 0;
    }

    if (!readdir(0) && closedir(0) < 0) {
        puts("dir bad handle: PASS");
    } else {
        puts("dir bad handle: FAIL");
        ok = 0;
    }

    puts(ok ? "dirprobe PASS" : "dirprobe FAIL");
    sys_exit(ok ? 0 : 1);
}
