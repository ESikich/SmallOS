#ifndef USER_SYS_WAIT_H
#define USER_SYS_WAIT_H

#include "types.h"
#include "../user_syscall.h"

#define WNOHANG SYS_WAITPID_WNOHANG

#define WIFEXITED(status)   (((status) & 0x7F) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) (((status) & 0x7F) != 0)
#define WTERMSIG(status)    ((status) & 0x7F)

pid_t waitpid(pid_t pid, int* status, int options);

#endif /* USER_SYS_WAIT_H */
