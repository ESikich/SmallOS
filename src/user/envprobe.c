#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "unistd.h"

static int streq(const char* a, const char* b) {
    return a && b && strcmp(a, b) == 0;
}

int main(int argc, char** argv, char** envp) {
    int ok = 1;

    puts("envprobe start");
    if (argc < 2 || !streq(argv[1], "execve-env")) {
        puts("envprobe argv: FAIL");
        ok = 0;
    } else {
        puts("envprobe argv: PASS");
    }

    if (!envp || envp != environ) {
        puts("envprobe envp: FAIL");
        ok = 0;
    } else {
        puts("envprobe envp: PASS");
    }

    if (!streq(getenv("SMALLOS_ENVPROBE"), "ok")) {
        puts("envprobe getenv: FAIL");
        ok = 0;
    } else {
        puts("envprobe getenv: PASS");
    }

    if (!streq(getenv("PATH"), "/custom/bin")) {
        puts("envprobe path: FAIL");
        ok = 0;
    } else {
        puts("envprobe path: PASS");
    }

    puts(ok ? "envprobe PASS" : "envprobe FAIL");
    return ok ? 0 : 1;
}
