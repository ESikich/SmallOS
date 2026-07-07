#ifndef USER_UNISTD_WRAPPER_H
#define USER_UNISTD_WRAPPER_H

#include "sys/stat.h"
#include "sys/types.h"

#ifndef USER_SSIZE_T_DEFINED
typedef int ssize_t;
#define USER_SSIZE_T_DEFINED
#endif

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define STDIN_FILENO 0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#define _SC_CLK_TCK 1
#define _SC_PAGESIZE 2
#define _SC_PAGE_SIZE _SC_PAGESIZE

extern char** environ;
extern char* optarg;
extern int optind;
extern int opterr;
extern int optopt;

int close(int fd);
int read(int fd, void* buf, unsigned int len);
int write(int fd, const void* buf, unsigned int len);
int lseek(int fd, int offset, int whence);
int unlink(const char* path);
int unlinkat(int dirfd, const char* path, int flags);
int link(const char* oldpath, const char* newpath);
int linkat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, int flags);
int symlink(const char* target, const char* linkpath);
int symlinkat(const char* target, int newdirfd, const char* linkpath);
ssize_t readlink(const char* path, char* buf, size_t bufsiz);
ssize_t readlinkat(int dirfd, const char* path, char* buf, size_t bufsiz);
int rename(const char* src, const char* dst);
int renameat(int olddirfd, const char* oldpath, int newdirfd, const char* newpath);
int access(const char* path, int mode);
int fstat(int fd, struct stat* st);
char* getcwd(char* buf, unsigned int size);
int chdir(const char* path);
int fchdir(int fd);
int chroot(const char* path);
int fsync(int fd);
int ftruncate(int fd, off_t length);
int truncate(const char* path, off_t length);
int remove(const char* path);
int pipe(int fds[2]);
int pipe2(int fds[2], int flags);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int dup3(int oldfd, int newfd, int flags);
pid_t fork(void);
pid_t vfork(void);
int execve(const char* path, char* const argv[], char* const envp[]);
int execv(const char* path, char* const argv[]);
int execvp(const char* file, char* const argv[]);
int execlp(const char* file, const char* arg, ...);
int getgroups(int size, gid_t list[]);
uid_t getuid(void);
uid_t geteuid(void);
gid_t getgid(void);
gid_t getegid(void);
int setuid(uid_t uid);
int setgid(gid_t gid);
int seteuid(uid_t euid);
int setegid(gid_t egid);
pid_t getpid(void);
pid_t getppid(void);
pid_t setsid(void);
pid_t getsid(pid_t pid);
int setpgid(pid_t pid, pid_t pgid);
pid_t getpgid(pid_t pid);
pid_t getpgrp(void);
int setpgrp(void);
int usleep(unsigned int usec);
unsigned int sleep(unsigned int seconds);
unsigned int alarm(unsigned int seconds);
void sync(void);
int isatty(int fd);
int gethostname(char* name, size_t len);
int sethostname(const char* name, size_t len);
int getopt(int argc, char* const argv[], const char* optstring);
long sysconf(int name);
__attribute__((noreturn)) void _exit(int status);

#endif
