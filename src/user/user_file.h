#ifndef USER_FILE_H
#define USER_FILE_H

#include "user_syscall.h"

typedef struct {
    int fd;
    int writable;
    uint32_t size;
} u_file_t;

static inline int u_file_open_read(u_file_t* f, const char* path) {
    if (!f) return -1;
    f->fd = sys_open(path);
    f->writable = 0;
    f->size = 0;
    if (f->fd < 0) {
        return -1;
    }
    if (sys_stat(path, &f->size, 0) < 0) {
        sys_close(f->fd);
        f->fd = -1;
        return -1;
    }
    return 0;
}

static inline int u_file_open_write(u_file_t* f, const char* path) {
    if (!f) return -1;
    f->fd = sys_open_write(path);
    f->writable = 1;
    f->size = 0;
    return f->fd < 0 ? -1 : 0;
}

static inline int u_file_close(u_file_t* f) {
    if (!f || f->fd < 0) return -1;
    int r = sys_close(f->fd);
    f->fd = -1;
    f->writable = 0;
    f->size = 0;
    return r;
}

static inline int u_file_read(u_file_t* f, void* buf, uint32_t len) {
    if (!f || f->fd < 0 || f->writable) return -1;
    return sys_fread(f->fd, (char*)buf, len);
}

static inline int u_file_write(u_file_t* f, const void* buf, uint32_t len) {
    if (!f || f->fd < 0 || !f->writable) return -1;
    int r = sys_writefd(f->fd, (const char*)buf, len);
    if (r > 0) {
        f->size = (uint32_t)sys_lseek(f->fd, 0, 1);
    }
    return r;
}

static inline int u_file_seek(u_file_t* f, int offset, int whence) {
    if (!f || f->fd < 0) return -1;
    return sys_lseek(f->fd, offset, whence);
}

static inline int u_file_stat(const char* path, uint32_t* size, int* is_dir) {
    return sys_stat(path, size, is_dir);
}

static inline int u_file_delete(const char* path) {
    return sys_unlink(path);
}

static inline int u_file_rename(const char* src, const char* dst) {
    return sys_rename(src, dst);
}

#endif
