#ifndef USER_SYS_STAT_WRAPPER_H
#define USER_SYS_STAT_WRAPPER_H

#include "../user_lib.h"
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
};

typedef unsigned int mode_t;

#define S_IFREG 0100000
#define S_IFDIR  0040000
#define S_IFLNK  0120000
#define S_IFMT   0170000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)
#define S_ISLNK(m) (((m) & S_IFMT) == S_IFLNK)

#define S_IRUSR 0400
#define S_IWUSR 0200
#define S_IXUSR 0100
#define S_IRGRP 0040
#define S_IWGRP 0020
#define S_IXGRP 0010
#define S_IROTH 0004
#define S_IWOTH 0002
#define S_IXOTH 0001

int stat(const char* path, struct stat* st);
int fstat(int fd, struct stat* st);
int lstat(const char* path, struct stat* st);
int mkdir(const char* path, mode_t mode);
int rmdir(const char* path);

#endif
