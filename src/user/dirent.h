#ifndef USER_DIRENT_H
#define USER_DIRENT_H

#include "user_syscall.h"

#ifndef NAME_MAX
#define NAME_MAX 255
#endif

struct dirent {
    char d_name[NAME_MAX + 1];
    unsigned int d_size;
    int d_is_dir;
};

typedef struct DIR {
    char path[256];
    unsigned int index;
    struct dirent current;
} DIR;

DIR *opendir(const char* path);
struct dirent *readdir(DIR* dirp);
int closedir(DIR* dirp);

#endif /* USER_DIRENT_H */
