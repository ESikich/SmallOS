#include "vfs.h"
#include "fat16.h"
#include "klib.h"
#include "pmm.h"
#include "uapi_poll.h"

#define VFS_FILE_CACHE_MAX_BYTES (PROCESS_FD_CACHE_PAGES * 4096u)

static u8 s_write_flush_buf[VFS_FILE_CACHE_MAX_BYTES];

static void vfs_file_cache_free(fd_entry_t* ent) {
    if (!ent) return;

    for (unsigned int i = 0; i < ent->cache_page_count; i++) {
        if (ent->cache_pages[i]) {
            pmm_free_frame(ent->cache_pages[i]);
            ent->cache_pages[i] = 0;
        }
    }
    ent->cache_page_count = 0;
}

static int vfs_file_ensure_capacity(fd_entry_t* ent, unsigned int bytes) {
    unsigned int needed_pages = (bytes + 4095u) / 4096u;
    while (ent->cache_page_count < needed_pages) {
        if (ent->cache_page_count >= PROCESS_FD_CACHE_PAGES) {
            return 0;
        }
        u32 frame = pmm_alloc_frame();
        if (!frame) {
            return 0;
        }
        k_memset((void*)frame, 0, 4096u);
        ent->cache_pages[ent->cache_page_count++] = frame;
    }
    return 1;
}

static int vfs_file_load_cache(fd_entry_t* ent) {
    if (!ent) return 0;
    if (ent->cache_page_count != 0) return 1;
    if (ent->size == 0) return 1;
    if (ent->size > VFS_FILE_CACHE_MAX_BYTES) return 0;

    u32 loaded_size = 0;
    const u8* data = fat16_load(ent->name, &loaded_size);
    if (!data) return 0;
    if (loaded_size != ent->size) return 0;

    u32 pages = (ent->size + 4095u) / 4096u;
    ent->cache_page_count = 0;
    for (u32 i = 0; i < pages; i++) {
        u32 frame = pmm_alloc_frame();
        if (!frame) {
            vfs_file_cache_free(ent);
            return 0;
        }
        ent->cache_pages[i] = frame;
        ent->cache_page_count++;
    }

    for (u32 i = 0; i < pages; i++) {
        u32 remaining = ent->size - (i * 4096u);
        u32 chunk = remaining < 4096u ? remaining : 4096u;
        u8* dst = (u8*)ent->cache_pages[i];
        const u8* src = data + (i * 4096u);
        for (u32 j = 0; j < chunk; j++) {
            dst[j] = src[j];
        }
    }

    return 1;
}

static int vfs_file_read(fd_entry_t* ent, char* buf, unsigned int len) {
    unsigned int remaining;
    unsigned int to_copy;
    unsigned int src_off;

    if (!ent || !ent->valid || ent->writable) return -1;
    if (len == 0) return 0;
    if (ent->offset >= ent->size) return 0;

    if (!vfs_file_load_cache(ent)) return -1;

    remaining = ent->size - ent->offset;
    to_copy = (len < remaining) ? len : remaining;
    src_off = ent->offset;

    for (u32 i = 0; i < to_copy; i++) {
        u32 page_idx = (src_off + i) / 4096u;
        u32 page_off = (src_off + i) % 4096u;
        const u8* src = (const u8*)ent->cache_pages[page_idx];
        buf[i] = (char)src[page_off];
    }

    ent->offset += to_copy;
    return (int)to_copy;
}

static int vfs_file_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    if (!ent || !ent->valid || !ent->writable) return -1;
    if (len == 0) return 0;

    unsigned int end = ent->offset + len;
    if (end < ent->offset) return -1;
    if (!vfs_file_ensure_capacity(ent, end)) return -1;

    for (unsigned int i = 0; i < len; i++) {
        unsigned int pos = ent->offset + i;
        unsigned int page_idx = pos / 4096u;
        unsigned int page_off = pos % 4096u;
        u8* dst = (u8*)ent->cache_pages[page_idx];
        dst[page_off] = (u8)buf[i];
    }

    ent->offset = end;
    if (ent->offset > ent->size) {
        ent->size = ent->offset;
    }
    return (int)len;
}

static int vfs_file_seek(fd_entry_t* ent, int offset, int whence) {
    unsigned int base;
    int new_off;

    if (!ent || !ent->valid) return -1;

    if (whence == 0) {
        base = 0;
    } else if (whence == 1) {
        base = ent->offset;
    } else if (whence == 2) {
        base = ent->size;
    } else {
        return -1;
    }

    new_off = (int)base + offset;
    if (new_off < 0) return -1;
    ent->offset = (unsigned int)new_off;
    return new_off;
}

static short vfs_file_poll(fd_entry_t* ent, short events) {
    short revents = 0;

    if (!ent || !ent->valid) return POLLERR;
    if ((events & POLLIN) && !ent->writable) {
        revents |= POLLIN;
    }
    if ((events & POLLOUT) && ent->writable) {
        revents |= POLLOUT;
    }
    return revents;
}

static int vfs_file_flush(fd_entry_t* ent) {
    if (!ent || !ent->valid || !ent->writable) {
        return 0;
    }
    if (ent->size > VFS_FILE_CACHE_MAX_BYTES) {
        return 0;
    }

    for (u32 i = 0; i < ent->size; i++) {
        u32 page_idx = i / 4096u;
        u32 page_off = i % 4096u;
        s_write_flush_buf[i] = ((u8*)ent->cache_pages[page_idx])[page_off];
    }

    return fat16_write_path(ent->name, s_write_flush_buf, ent->size);
}

static void vfs_file_close(fd_entry_t* ent) {
    if (!ent) return;
    vfs_file_cache_free(ent);
    k_memset(ent, 0, sizeof(*ent));
}

static const process_handle_ops_t s_file_ops = {
    .read = vfs_file_read,
    .write = vfs_file_write,
    .seek = vfs_file_seek,
    .poll = vfs_file_poll,
    .flush = vfs_file_flush,
    .close = vfs_file_close,
};

const process_handle_ops_t* vfs_file_ops(void) {
    return &s_file_ops;
}

void vfs_file_init(fd_entry_t* ent, const char* path, u32 size, int writable) {
    if (!ent || !path) return;

    ent->kind = PROCESS_HANDLE_KIND_FILE;
    ent->ops = &s_file_ops;
    ent->writable = writable ? 1 : 0;
    ent->size = size;
    ent->offset = 0;
    ent->cache_page_count = 0;
    k_memcpy(ent->name, path, (k_size_t)k_strlen(path) + 1u);
}

const u8* vfs_load_file(const char* path, u32* out_size) {
    return fat16_load(path, out_size);
}

int vfs_stat(const char* path, u32* out_size, int* out_is_dir) {
    u32 size = 0;

    if (fat16_stat(path, &size)) {
        if (out_size) {
            *out_size = size;
        }
        if (out_is_dir) {
            *out_is_dir = 0;
        }
        return 1;
    }

    if (!fat16_is_dir(path)) {
        return 0;
    }

    if (out_size) {
        *out_size = 0;
    }
    if (out_is_dir) {
        *out_is_dir = 1;
    }
    return 1;
}

int vfs_is_dir(const char* path) {
    return fat16_is_dir(path);
}

int vfs_write_root(const char* name, const u8* data, u32 size) {
    return fat16_write(name, data, size);
}

int vfs_write_path(const char* path, const u8* data, u32 size) {
    return fat16_write_path(path, data, size);
}

int vfs_unlink(const char* path) {
    return fat16_rm(path);
}

int vfs_rename(const char* src, const char* dst) {
    return fat16_move(src, dst);
}

int vfs_mkdir(const char* path) {
    return fat16_mkdir(path);
}

int vfs_rmdir(const char* path) {
    return fat16_rmdir(path);
}

int vfs_dirent_at(const char* path,
                  u32 index,
                  char* out_name,
                  u32 out_name_size,
                  u32* out_size,
                  int* out_is_dir) {
    return fat16_dirent_at(path, index, out_name, out_name_size,
                           out_size, out_is_dir);
}
