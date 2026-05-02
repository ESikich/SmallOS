#include "user_lib.h"

int main(int argc, char** argv);

/*
 * Generic hosted-ish adapter: the kernel still enters _start(argc, argv),
 * while user programs can define main(argc, argv) and return an exit status.
 */
void _start(int argc, char** argv) {
    sys_exit(main(argc, argv));
}
