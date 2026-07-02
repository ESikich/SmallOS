#include "unistd.h"
#include "stdio.h"
#include "errno.h"

int main(int argc, char** argv) {
    static char* sh_argv[18];
    int out_argc = 0;

    sh_argv[out_argc++] = "sh";
    for (int i = 1; i < argc && out_argc < 17; i++) {
        sh_argv[out_argc++] = argv[i];
    }
    sh_argv[out_argc] = 0;

    execve("/usr/bin/busybox", sh_argv, environ);
    perror("sh");
    return errno ? errno : 127;
}
