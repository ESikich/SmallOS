#ifndef USER_SYS_WAIT_H
#define USER_SYS_WAIT_H

#include "types.h"
#include "uapi_syscall.h"

#define WNOHANG SYS_WAITPID_WNOHANG
#define WUNTRACED SYS_WAITPID_WUNTRACED

#define WIFEXITED(status)   (((status) & 0x7F) == 0)
#define WEXITSTATUS(status) (((status) >> 8) & 0xFF)
#define WIFSIGNALED(status) (((status) & 0x7F) != 0)
#define WTERMSIG(status)    ((status) & 0x7F)
#define WIFSTOPPED(status)  (((status) & 0xFF) == 0x7F)
#define WSTOPSIG(status)    (((status) >> 8) & 0xFF)
#define WCOREDUMP(status)   (0)

pid_t waitpid(pid_t pid, int* status, int options);
pid_t wait(int* status);

#endif /* USER_SYS_WAIT_H */
