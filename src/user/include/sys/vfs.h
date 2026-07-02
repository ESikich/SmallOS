#ifndef USER_SYS_VFS_H
#define USER_SYS_VFS_H

typedef long fsblkcnt_t;
typedef long fsfilcnt_t;

struct statfs {
    long f_type;
    long f_bsize;
    fsblkcnt_t f_blocks;
    fsblkcnt_t f_bfree;
    fsblkcnt_t f_bavail;
    fsfilcnt_t f_files;
    fsfilcnt_t f_ffree;
    long f_fsid;
    long f_namelen;
    long f_frsize;
    long f_flags;
};

int statfs(const char* path, struct statfs* buf);
int fstatfs(int fd, struct statfs* buf);

#endif /* USER_SYS_VFS_H */
