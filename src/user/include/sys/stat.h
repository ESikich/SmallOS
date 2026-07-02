#ifndef USER_SYS_STAT_WRAPPER_H
#define USER_SYS_STAT_WRAPPER_H

#include "types.h"
#include "../time.h"

struct stat {
    unsigned long st_dev;
    unsigned long st_ino;
    unsigned long st_nlink;
    unsigned int st_mode;
    unsigned int st_uid;
    unsigned int st_gid;
    unsigned long st_rdev;
    long st_size;
    long st_blksize;
    long st_blocks;
    time_t st_atime;
    time_t st_mtime;
    time_t st_ctime;
    struct timespec st_atim;
    struct timespec st_mtim;
    struct timespec st_ctim;
};

#ifndef USER_MODE_T_DEFINED
typedef unsigned int mode_t;
#define USER_MODE_T_DEFINED
#endif

#define S_IFIFO  0010000
#define S_IFCHR  0020000
#define S_IFDIR  0040000
#define S_IFBLK  0060000
#define S_IFREG 0100000
#define S_IFLNK 0120000
#define S_IFSOCK 0140000
#define S_IFMT  0170000
#define S_ISFIFO(m) (((m) & S_IFMT) == S_IFIFO)
#define S_ISCHR(m) (((m) & S_IFMT) == S_IFCHR)
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISBLK(m) (((m) & S_IFMT) == S_IFBLK)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)
#define S_ISSOCK(m) (((m) & S_IFMT) == S_IFSOCK)

#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRWXU 0700
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IRWXG 0070
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001
#define S_IRWXO 0007
#define S_ISUID 04000
#define S_ISGID 02000
#define S_ISVTX 01000
#define S_IREAD S_IRUSR
#define S_IWRITE S_IWUSR

int stat(const char* path, struct stat* st);
int fstat(int fd, struct stat* st);
int fstatat(int dirfd, const char* path, struct stat* st, int flags);
int lstat(const char* path, struct stat* st);
int mkdir(const char* path, mode_t mode);
int mkdirat(int dirfd, const char* path, mode_t mode);
int rmdir(const char* path);
int chmod(const char* path, mode_t mode);
int chown(const char* path, uid_t owner, gid_t group);
int lchown(const char* path, uid_t owner, gid_t group);
int fchmod(int fd, mode_t mode);
int fchown(int fd, uid_t owner, gid_t group);
int mknod(const char* path, mode_t mode, dev_t dev);
int mkfifo(const char* path, mode_t mode);
int utimensat(int dirfd, const char* pathname, const struct timespec times[2], int flags);
int futimens(int fd, const struct timespec times[2]);
mode_t umask(mode_t mask);

#endif
