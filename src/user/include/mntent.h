#ifndef USER_MNTENT_H
#define USER_MNTENT_H

#include "stdio.h"

#define MOUNTED "/proc/mounts"
#define MNTTAB "/proc/mounts"

struct mntent {
    char* mnt_fsname;
    char* mnt_dir;
    char* mnt_type;
    char* mnt_opts;
    int mnt_freq;
    int mnt_passno;
};

FILE* setmntent(const char* filename, const char* type);
struct mntent* getmntent(FILE* stream);
int endmntent(FILE* stream);
char* hasmntopt(const struct mntent* mnt, const char* opt);

#endif /* USER_MNTENT_H */
