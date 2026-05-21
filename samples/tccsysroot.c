#include <stdio.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char** argv) {
    int name_len = argv && argv[0] ? (int)strlen(argv[0]) : 0;
    int total = argc + name_len + STDOUT_FILENO;
    printf("tcc sysroot ok: argc=%d stdout=%d name_len=%d total=%d\n",
           argc, STDOUT_FILENO, name_len, total);
    return 0;
}
