#include "user_lib.h"

extern char** environ;
void __smallos_set_environ(char** envp);
__attribute__((noreturn)) void exit(int code);
int main(int argc, char** argv, char** envp);

/*
 * Generic hosted-ish adapter. The kernel enters _start(argc, argv, envp);
 * two-argument main() definitions still work because the extra cdecl argument
 * is ignored by callees that do not declare it.
 */
void _start(int argc, char** argv, char** envp) {
    environ = envp;
    __smallos_set_environ(envp);
    exit(main(argc, argv, envp));
}
