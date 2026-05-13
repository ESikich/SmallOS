#include "unistd.h"
#include "stdio.h"

int main(void) {
    char* argv[] = { "usr/bin/hello", "execve", 0 };
    execve("usr/bin/hello", argv, 0);
    puts("execveprobe: FAIL");
    return 1;
}
