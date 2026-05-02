#ifndef USER_UNISTD_WRAPPER_H
#define USER_UNISTD_WRAPPER_H

#include "user_syscall.h"
#include "sys/stat.h"

typedef int ssize_t;

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

int close(int fd);
int read(int fd, void* buf, unsigned int len);
int write(int fd, const void* buf, unsigned int len);
int lseek(int fd, int offset, int whence);
int unlink(const char* path);
int rename(const char* src, const char* dst);
int access(const char* path, int mode);
int fstat(int fd, struct stat* st);
char* getcwd(char* buf, unsigned int size);
int chdir(const char* path);
int remove(const char* path);
int execvp(const char* file, char* const argv[]);

#endif
