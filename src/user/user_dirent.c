#include "user_lib.h"
#include "dirent.h"
#include "errno.h"
#include "string.h"

#define USER_DIRENT_POOL_SIZE 4

static DIR s_dir_pool[USER_DIRENT_POOL_SIZE];
static unsigned char s_dir_pool_used[USER_DIRENT_POOL_SIZE];

static int dir_init(DIR* d, const char* path) {
    memset(d, 0, sizeof(*d));
    strncpy(d->path, path, sizeof(d->path) - 1u);
    d->batch = (struct dirent*)malloc(sizeof(struct dirent) * DIRENT_BATCH_SIZE);
    if (!d->batch) {
        errno = ENOMEM;
        return 0;
    }
    return 1;
}

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
            if (!dir_init(d, use_path)) {
                s_dir_pool_used[i] = 0;
                return 0;
            }
            return d;
        }
    }

    {
        DIR* d = (DIR*)malloc(sizeof(DIR));
        if (!d) {
            errno = ENOMEM;
            return 0;
        }
        if (!dir_init(d, use_path)) {
            free(d);
            return 0;
        }
        return d;
    }
}

struct dirent* readdir(DIR* dirp) {
    int rc;
    if (!dirp) {
        errno = EBADF;
        return 0;
    }

    if (dirp->batch_pos >= dirp->batch_count) {
        if (!dirp->batch) {
            errno = EBADF;
            return 0;
        }
        rc = sys_dirlist_batch(dirp->path, dirp->index,
                               dirp->batch, DIRENT_BATCH_SIZE);
        if (rc < 0) {
            errno = -rc;
            return 0;
        }
        if (rc == 0) return 0;
        dirp->batch_count = (unsigned int)rc;
        dirp->batch_pos = 0;
        dirp->index += (unsigned int)rc;
    }

    dirp->current = dirp->batch[dirp->batch_pos++];
    return &dirp->current;
}

int closedir(DIR* dirp) {
    if (!dirp) {
        errno = EBADF;
        return -1;
    }
    for (unsigned int i = 0; i < USER_DIRENT_POOL_SIZE; i++) {
        if (dirp == &s_dir_pool[i]) {
            if (dirp->batch) {
                free(dirp->batch);
                dirp->batch = 0;
            }
            s_dir_pool_used[i] = 0;
            return 0;
        }
    }
    if (dirp->batch) {
        free(dirp->batch);
    }
    free(dirp);
    return 0;
}
