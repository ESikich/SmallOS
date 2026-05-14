#include "unistd.h"
#include "stdio.h"

int main(void) {
    char* argv[] = { "usr/libexec/tests/envprobe", "execve-env", 0 };
    char* envp[] = {
        "PATH=/custom/bin",
        "SMALLOS_ENVPROBE=ok",
        0
    };
    execve("usr/libexec/tests/envprobe", argv, envp);
    puts("execveprobe: FAIL");
    return 1;
}
