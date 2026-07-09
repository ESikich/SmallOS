#include "unistd.h"
#include "stdio.h"

int main(void) {
    char* argv[] = {
        "usr/libexec/tests/envprobe",
        "execve-env",
        "arg-budget-00",
        "arg-budget-01",
        "arg-budget-02",
        "arg-budget-03",
        "arg-budget-04",
        "arg-budget-05",
        "arg-budget-06",
        "arg-budget-07",
        "arg-budget-08",
        "arg-budget-09",
        "arg-budget-10",
        "arg-budget-11",
        "arg-budget-12",
        "arg-budget-13",
        "arg-budget-14",
        "arg-budget-15",
        "arg-budget-16",
        "arg-budget-17",
        "arg-budget-18",
        "arg-budget-19",
        0
    };
    char* envp[] = {
        "PATH=/custom/bin",
        "SMALLOS_ENVPROBE=ok",
        "SMALLOS_ENV_BUDGET=abcdefghijklmnopqrstuvwxyz0123456789-abcdefghijklmnopqrstuvwxyz0123456789",
        0
    };
    execve("usr/libexec/tests/envprobe", argv, envp);
    puts("execveprobe: FAIL");
    return 1;
}
