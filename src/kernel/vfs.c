#include "vfs.h"
#include "fat16.h"
#include "klib.h"
#include "paging.h"
#include "pmm.h"
#include "scheduler.h"
#include "uapi_poll.h"
#include "uapi_errno.h"

#define VFS_FILE_CACHE_MAX_BYTES (PROCESS_FD_CACHE_PAGES * 4096u)

typedef struct {
    fd_entry_t* ent;
} vfs_page_source_t;

typedef struct {
    fd_entry_t* ent;
} vfs_page_sink_t;

static int vfs_page_sink_write(void* ctx, u32 offset, const u8* data, u32 len);

static void vfs_zero_frame(u32 frame) {
    k_memset(paging_phys_to_kernel_virt(frame), 0, 4096u);
}

static int vfs_cache_page_frame(fd_entry_t* ent, u32 page_idx, u32* out_frame) {
    if (!ent || !out_frame || !ent->cache_pages_frame ||
        page_idx >= ent->cache_page_count) {
        return 0;
    }

    u32* cache_pages = (u32*)paging_phys_to_kernel_virt(ent->cache_pages_frame);
    *out_frame = cache_pages[page_idx];

    return *out_frame ? 1 : 0;
}

static int vfs_set_cache_page_frame(fd_entry_t* ent, u32 page_idx, u32 frame) {
    if (!ent || !ent->cache_pages_frame || page_idx >= PROCESS_FD_CACHE_PAGES) {
        return 0;
    }

    u32* cache_pages = (u32*)paging_phys_to_kernel_virt(ent->cache_pages_frame);
    cache_pages[page_idx] = frame;

    return 1;
}

static int vfs_copy_from_cache(fd_entry_t* ent, u32 offset, u8* out, u32 len) {
    u32 copied = 0;

    if (!ent || !out) return 0;
    if (offset > ent->size || len > ent->size - offset) return 0;

    while (copied < len) {
        u32 pos = offset + copied;
        u32 page_idx = pos / 4096u;
        u32 page_off = pos % 4096u;
        u32 chunk = 4096u - page_off;
        u32 frame = 0;

        if (chunk > len - copied) chunk = len - copied;
        if (!vfs_cache_page_frame(ent, page_idx, &frame)) {
            return 0;
        }

        const u8* page = (const u8*)paging_phys_to_kernel_virt(frame);
        k_memcpy(out + copied, page + page_off, chunk);

        copied += chunk;
    }

    return 1;
}

static int vfs_copy_to_cache(fd_entry_t* ent, u32 offset, const u8* data, u32 len) {
    u32 copied = 0;
    u32 capacity;

    if (!ent || !data) return 0;
    capacity = ent->cache_page_count * 4096u;
    if (offset > capacity || len > capacity - offset) return 0;

    while (copied < len) {
        u32 pos = offset + copied;
        u32 page_idx = pos / 4096u;
        u32 page_off = pos % 4096u;
        u32 chunk = 4096u - page_off;
        u32 frame = 0;

        if (chunk > len - copied) chunk = len - copied;
        if (!vfs_cache_page_frame(ent, page_idx, &frame)) {
            return 0;
        }

        u8* page = (u8*)paging_phys_to_kernel_virt(frame);
        k_memcpy(page + page_off, data + copied, chunk);

        copied += chunk;
    }

    return 1;
}

static int vfs_file_ensure_page_table(fd_entry_t* ent) {
    if (!ent) return 0;
    if (ent->cache_pages_frame) return 1;

    u32 frame = pmm_alloc_frame();
    if (!frame) {
        return 0;
    }
    vfs_zero_frame(frame);
    ent->cache_pages_frame = frame;
    return 1;
}

static void vfs_file_cache_free(fd_entry_t* ent) {
    if (!ent) return;

    for (unsigned int i = 0; i < ent->cache_page_count; i++) {
        u32 frame = 0;
        if (vfs_cache_page_frame(ent, i, &frame)) {
            pmm_free_frame(frame);
            vfs_set_cache_page_frame(ent, i, 0);
        }
    }
    ent->cache_page_count = 0;
    if (ent->cache_pages_frame) {
        pmm_free_frame(ent->cache_pages_frame);
        ent->cache_pages_frame = 0;
    }
}

static int vfs_file_ensure_capacity(fd_entry_t* ent, unsigned int bytes) {
    unsigned int needed_pages = (bytes + 4095u) / 4096u;
    if (needed_pages > PROCESS_FD_CACHE_PAGES) {
        return 0;
    }
    if (needed_pages > 0 && !vfs_file_ensure_page_table(ent)) {
        return 0;
    }
    while (ent->cache_page_count < needed_pages) {
        u32 frame = pmm_alloc_frame();
        if (!frame) {
            return 0;
        }
        vfs_zero_frame(frame);
        if (!vfs_set_cache_page_frame(ent, ent->cache_page_count, frame)) {
            pmm_free_frame(frame);
            return 0;
        }
        ent->cache_page_count++;
    }
    return 1;
}

static int vfs_file_load_cache(fd_entry_t* ent) {
    if (!ent) return 0;
    if (ent->cache_page_count != 0) return 1;
    if (ent->size == 0) return 1;
    if (ent->size > VFS_FILE_CACHE_MAX_BYTES) return 0;

    if (!vfs_file_ensure_capacity(ent, ent->size)) {
        return 0;
    }

    u32 loaded_size = 0;
    vfs_page_sink_t page_sink = { ent };
    fat16_data_sink_t sink = { &page_sink, vfs_page_sink_write };

    if (!fat16_read_path_to_sink(ent->name, &sink, &loaded_size)) {
        vfs_file_cache_free(ent);
        return 0;
    }
    if (loaded_size != ent->size) {
        vfs_file_cache_free(ent);
        return 0;
    }

    return 1;
}

static int vfs_page_sink_write(void* ctx, u32 offset, const u8* data, u32 len) {
    vfs_page_sink_t* sink = (vfs_page_sink_t*)ctx;
    fd_entry_t* ent = sink ? sink->ent : 0;

    if (!ent || !data) return 0;
    return vfs_copy_to_cache(ent, offset, data, len);
}

static int vfs_file_read(fd_entry_t* ent, char* buf, unsigned int len) {
    u32 read_len = 0;

    if (!ent || !ent->valid || !ent->readable) return -EBADF;
    if (len == 0) return 0;
    if (ent->offset >= ent->size) return 0;

    if (ent->size <= VFS_FILE_CACHE_MAX_BYTES) {
        unsigned int remaining;
        unsigned int to_copy;
        unsigned int src_off;

        if (!vfs_file_load_cache(ent)) return -EIO;

        remaining = ent->size - ent->offset;
        to_copy = (len < remaining) ? len : remaining;
        src_off = ent->offset;

        if (!vfs_copy_from_cache(ent, src_off, (u8*)buf, to_copy)) return -EIO;

        ent->offset += to_copy;
        return (int)to_copy;
    }

    if (!fat16_read_at_path(ent->name, ent->offset, (u8*)buf, len, &read_len)) {
        return -EIO;
    }

    ent->offset += read_len;
    return (int)read_len;
}

static int vfs_file_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    if (!ent || !ent->valid || !ent->writable) return -EBADF;
    if (len == 0) return 0;

    unsigned int end = ent->offset + len;
    if (end < ent->offset) return -EFBIG;
    if (!fat16_write_at_path(ent->name, ent->offset, (const u8*)buf, len, &ent->size, 1)) {
        return -EIO;
    }

    ent->offset = end;
    vfs_file_cache_free(ent);
    ent->dirty = 0;
    return (int)len;
}

static int vfs_page_source_read(void* ctx, u32 offset, u8* out, u32 len) {
    vfs_page_source_t* source = (vfs_page_source_t*)ctx;
    fd_entry_t* ent = source ? source->ent : 0;

    if (!ent || !out) return 0;
    if (offset > ent->size || len > ent->size - offset) return 0;

    return vfs_copy_from_cache(ent, offset, out, len);
}

static int vfs_file_seek(fd_entry_t* ent, int offset, int whence) {
    unsigned int base;
    int new_off;

    if (!ent || !ent->valid) return -EBADF;

    if (whence == 0) {
        base = 0;
    } else if (whence == 1) {
        base = ent->offset;
    } else if (whence == 2) {
        base = ent->size;
    } else {
        return -EINVAL;
    }

    new_off = (int)base + offset;
    if (new_off < 0) return -EINVAL;
    ent->offset = (unsigned int)new_off;
    return new_off;
}

static short vfs_file_poll(fd_entry_t* ent, short events) {
    short revents = 0;

    if (!ent || !ent->valid) return POLLERR;
    if ((events & POLLIN) && ent->readable) {
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
    if (!ent->dirty) {
        return 1;
    }
    if (ent->size > VFS_FILE_CACHE_MAX_BYTES) {
        return 0;
    }

    vfs_page_source_t page_source = { ent };
    fat16_data_source_t source = { &page_source, vfs_page_source_read };

    if (!fat16_write_path_from_source(ent->name, &source, ent->size)) {
        return 0;
    }
    ent->dirty = 0;
    return 1;
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

void vfs_file_init(fd_entry_t* ent, const char* path, u32 size, int readable, int writable) {
    if (!ent || !path) return;

    ent->kind = PROCESS_HANDLE_KIND_FILE;
    ent->ops = &s_file_ops;
    ent->readable = readable ? 1 : 0;
    ent->writable = writable ? 1 : 0;
    ent->dirty = 0;
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
