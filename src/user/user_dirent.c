#include "user_lib.h"
#include "dirent.h"
#include "errno.h"
#include "string.h"

#define USER_DIRENT_POOL_SIZE 4

static DIR s_dir_pool[USER_DIRENT_POOL_SIZE];
static unsigned char s_dir_pool_used[USER_DIRENT_POOL_SIZE];

DIR* opendir(const char* path) {
    const char* use_path = (!path || !*path) ? "/" : path;
    uint32_t size = 0;
    int is_dir = 0;

    int sr = sys_stat(use_path, &size, &is_dir);
    if (sr < 0) {
        errno = -sr;
        return 0;
    }
    if (!is_dir) {
        errno = ENOTDIR;
        return 0;
    }

    for (unsigned int i = 0; i < USER_DIRENT_POOL_SIZE; i++) {
        if (!s_dir_pool_used[i]) {
            DIR* d = &s_dir_pool[i];
            s_dir_pool_used[i] = 1;
            memset(d, 0, sizeof(*d));
            strncpy(d->path, use_path, sizeof(d->path) - 1u);
            d->index = 0;
            return d;
        }
    }

    {
        DIR* d = (DIR*)malloc(sizeof(DIR));
        if (!d) {
            errno = ENOMEM;
            return 0;
        }
        memset(d, 0, sizeof(*d));
        strncpy(d->path, use_path, sizeof(d->path) - 1u);
        d->index = 0;
        return d;
    }
}

struct dirent* readdir(DIR* dirp) {
    int rc;
    if (!dirp) {
        errno = EBADF;
        return 0;
    }
    rc = sys_dirlist(dirp->path, dirp->index, &dirp->current);
    if (rc < 0) {
        errno = -rc;
        return 0;
    }
    if (rc == 0) return 0;
    dirp->index++;
    return &dirp->current;
}

int closedir(DIR* dirp) {
    if (!dirp) {
        errno = EBADF;
        return -1;
    }
    for (unsigned int i = 0; i < USER_DIRENT_POOL_SIZE; i++) {
        if (dirp == &s_dir_pool[i]) {
            s_dir_pool_used[i] = 0;
            return 0;
        }
    }
    free(dirp);
    return 0;
}
