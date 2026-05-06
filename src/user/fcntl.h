#ifndef USER_FCNTL_WRAPPER_H
#define USER_FCNTL_WRAPPER_H

#include "uapi_syscall.h"

#define O_RDONLY 0x0000
#define O_WRONLY 0x0001
#define O_RDWR   0x0002
#define O_CREAT  0x0040
#define O_TRUNC  0x0200
#define O_APPEND 0x0400
#define O_NONBLOCK SYS_FD_FLAG_NONBLOCK
#define O_CLOEXEC 0x00080000u
#define O_BINARY 0

#define F_GETFL SYS_FCNTL_GETFL
#define F_SETFL SYS_FCNTL_SETFL
#define F_GETFD 1
#define F_SETFD 2

int open(const char* path, int flags, ...);
int fcntl(int fd, int cmd, ...);

#endif
