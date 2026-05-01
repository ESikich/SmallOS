#include "user_lib.h"

int main(int argc, char** argv);

void _start(int argc, char** argv) {
    sys_exit(main(argc, argv));
}
