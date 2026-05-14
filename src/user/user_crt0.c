#include "user_lib.h"

extern char** environ;
int main(int argc, char** argv, char** envp);

/*
 * Generic hosted-ish adapter. The kernel enters _start(argc, argv, envp);
 * two-argument main() definitions still work because the extra cdecl argument
 * is ignored by callees that do not declare it.
 */
void _start(int argc, char** argv, char** envp) {
    environ = envp;
    sys_exit(main(argc, argv, envp));
}
