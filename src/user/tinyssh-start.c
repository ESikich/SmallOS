#include "errno.h"
#include "stdio.h"
#include "stdlib.h"
#include "string.h"
#include "sys/stat.h"
#include "sys/wait.h"
#include "unistd.h"

static int exists(const char* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static int run_makekey(void) {
    pid_t pid = fork();
    int status = 0;
    char* const argv[] = {
        "/usr/sbin/tinysshd-makekey",
        "-q",
        "/etc/tinyssh/sshkeydir",
        0
    };
    char* const envp[] = { 0 };

    if (pid < 0) return -1;
    if (pid == 0) {
        execve(argv[0], argv, envp);
        _exit(127);
    }
    while (waitpid(pid, &status, 0) < 0) {
        if (errno != EINTR) return -1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        errno = EIO;
        return -1;
    }
    return 0;
}

void _start(void) {
    char* const argv[] = {
        "/usr/bin/busybox",
        "tcpsvd",
        "0",
        "22",
        "/usr/sbin/tinysshd",
        "-v",
        "/etc/tinyssh/sshkeydir",
        0
    };
    char* const envp[] = { 0 };

    mkdir("/etc", 0755);
    mkdir("/etc/tinyssh", 0755);
    if (!exists("/etc/tinyssh/sshkeydir/.ed25519.sk")) {
        if (run_makekey() < 0) {
            perror("tinysshd-makekey");
            exit(1);
        }
    }

    execve(argv[0], argv, envp);
    perror("busybox tcpsvd");
    exit(127);
}
