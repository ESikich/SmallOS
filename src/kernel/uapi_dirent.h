#ifndef UAPI_DIRENT_H
#define UAPI_DIRENT_H

typedef unsigned int uapi_dir_size_t;

#define UAPI_DIRENT_NAME_MAX 256u

typedef struct uapi_dirent {
    char  d_name[UAPI_DIRENT_NAME_MAX];
    unsigned int d_size;
    int   d_is_dir;
} uapi_dirent_t;

#endif /* UAPI_DIRENT_H */
