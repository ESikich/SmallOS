#include "shell/shell.h"
#include "user_syscall.h"

void _start(int argc, char** argv) {
    sys_exit(shell_main(argc, argv));
}
