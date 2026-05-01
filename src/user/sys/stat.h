#ifndef USER_SYS_STAT_WRAPPER_H
#define USER_SYS_STAT_WRAPPER_H

#include "../user_lib.h"

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
    unsigned long st_atime;
    unsigned long st_mtime;
    unsigned long st_ctime;
};

#define S_IFREG 0100000
#define S_IFDIR  0040000
#define S_IFMT   0170000
#define S_ISDIR(m) (((m) & S_IFMT) == S_IFDIR)
#define S_ISREG(m) (((m) & S_IFMT) == S_IFREG)

int stat(const char* path, struct stat* st);
int fstat(int fd, struct stat* st);

#endif
