#include "vfs.h"
#include "ext2.h"
#include "klib.h"
#include "paging.h"
#include "pmm.h"
#include "scheduler.h"
#include "uapi_poll.h"
#include "uapi_errno.h"

#define VFS_FILE_CACHE_MAX_BYTES (PROCESS_FD_CACHE_PAGES * 4096u)
#define VFS_RO_MAP_CACHE_PAGES 128u

typedef struct {
    fd_entry_t* ent;
} vfs_page_source_t;

typedef struct {
    fd_entry_t* ent;
} vfs_page_sink_t;

typedef struct vfs_file_object {
    unsigned int refs;
    int readable;
    int writable;
    int dirty;
    int is_dir;
    char name[PROCESS_FD_NAME_MAX];
    u32 size;
    u32 offset;
    u32 cache_page_count;
    u32 cache_pages_frame;
} vfs_file_object_t;

typedef struct vfs_ro_map_entry {
    int used;
    char name[PROCESS_FD_NAME_MAX];
    u32 ino;
    u32 size;
    u32 mtime;
    u32 ctime;
    u32 file_offset;
    u32 frame;
} vfs_ro_map_entry_t;

static vfs_ro_map_entry_t s_ro_map_cache[VFS_RO_MAP_CACHE_PAGES];

static int vfs_page_sink_write(void* ctx, u32 offset, const u8* data, u32 len);
static int vfs_file_flush(fd_entry_t* ent);
static unsigned int vfs_irq_save(void);
static void vfs_irq_restore(unsigned int flags);

static vfs_file_object_t* vfs_file_object(fd_entry_t* ent) {
    if (!ent || ent->kind != PROCESS_HANDLE_KIND_FILE || !ent->aux_frame) return 0;
    return (vfs_file_object_t*)paging_phys_to_kernel_virt(ent->aux_frame);
}

static void vfs_file_sync_entry(fd_entry_t* ent) {
    vfs_file_object_t* obj = vfs_file_object(ent);
    if (!ent || !obj) return;
    ent->readable = obj->readable;
    ent->writable = obj->writable;
    ent->dirty = obj->dirty;
    ent->is_dir = obj->is_dir;
    ent->size = obj->size;
    ent->offset = obj->offset;
    ent->cache_page_count = obj->cache_page_count;
    ent->cache_pages_frame = obj->cache_pages_frame;
}

static void vfs_zero_frame(u32 frame) {
    k_memset(paging_phys_to_kernel_virt(frame), 0, 4096u);
}

static void vfs_ro_map_drop_entry(vfs_ro_map_entry_t* ent) {
    if (!ent || !ent->used) return;
    if (ent->frame) {
        pmm_release_frame(ent->frame);
    }
    k_memset(ent, 0, sizeof(*ent));
}

static void vfs_ro_map_invalidate_path(const char* path) {
    if (!path) return;

    unsigned int flags = vfs_irq_save();
    for (unsigned int i = 0; i < VFS_RO_MAP_CACHE_PAGES; i++) {
        if (s_ro_map_cache[i].used &&
            k_strcmp(s_ro_map_cache[i].name, path) == 0) {
            vfs_ro_map_drop_entry(&s_ro_map_cache[i]);
        }
    }
    vfs_irq_restore(flags);
}

static vfs_ro_map_entry_t* vfs_ro_map_find(const char* path,
                                           const sys_stat_info_t* st,
                                           u32 file_offset) {
    for (unsigned int i = 0; i < VFS_RO_MAP_CACHE_PAGES; i++) {
        vfs_ro_map_entry_t* ent = &s_ro_map_cache[i];
        if (!ent->used) continue;
        if (ent->file_offset != file_offset) continue;
        if (ent->ino != st->ino || ent->size != st->size ||
            ent->mtime != st->mtime || ent->ctime != st->ctime) {
            continue;
        }
        if (k_strcmp(ent->name, path) == 0) return ent;
    }
    return 0;
}

static vfs_ro_map_entry_t* vfs_ro_map_victim(void) {
    vfs_ro_map_entry_t* first_free = 0;

    for (unsigned int i = 0; i < VFS_RO_MAP_CACHE_PAGES; i++) {
        vfs_ro_map_entry_t* ent = &s_ro_map_cache[i];
        if (!ent->used) {
            if (!first_free) first_free = ent;
            continue;
        }
        if (ent->frame && pmm_frame_refcount(ent->frame) <= 1u) {
            vfs_ro_map_drop_entry(ent);
            return ent;
        }
    }
    return first_free;
}

static int vfs_cache_page_frame(fd_entry_t* ent, u32 page_idx, u32* out_frame) {
    vfs_file_object_t* obj = vfs_file_object(ent);
    if (!obj || !out_frame || !obj->cache_pages_frame ||
        page_idx >= obj->cache_page_count) {
        return 0;
    }

    u32* cache_pages = (u32*)paging_phys_to_kernel_virt(obj->cache_pages_frame);
    *out_frame = cache_pages[page_idx];

    return *out_frame ? 1 : 0;
}

static int vfs_set_cache_page_frame(fd_entry_t* ent, u32 page_idx, u32 frame) {
    vfs_file_object_t* obj = vfs_file_object(ent);
    if (!obj || !obj->cache_pages_frame || page_idx >= PROCESS_FD_CACHE_PAGES) {
        return 0;
    }

    u32* cache_pages = (u32*)paging_phys_to_kernel_virt(obj->cache_pages_frame);
    cache_pages[page_idx] = frame;

    return 1;
}

static int vfs_copy_from_cache(fd_entry_t* ent, u32 offset, u8* out, u32 len) {
    u32 copied = 0;
    vfs_file_object_t* obj = vfs_file_object(ent);

    if (!obj || !out) return 0;
    if (offset > obj->size || len > obj->size - offset) return 0;

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
    vfs_file_object_t* obj = vfs_file_object(ent);

    if (!obj || !data) return 0;
    capacity = obj->cache_page_count * 4096u;
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
    vfs_file_object_t* obj = vfs_file_object(ent);
    if (!obj) return 0;
    if (obj->cache_pages_frame) return 1;

    u32 frame = pmm_alloc_frame();
    if (!frame) {
        return 0;
    }
    vfs_zero_frame(frame);
    obj->cache_pages_frame = frame;
    vfs_file_sync_entry(ent);
    return 1;
}

static void vfs_file_cache_free(fd_entry_t* ent) {
    vfs_file_object_t* obj = vfs_file_object(ent);
    if (!obj) return;

    for (unsigned int i = 0; i < obj->cache_page_count; i++) {
        u32 frame = 0;
        if (vfs_cache_page_frame(ent, i, &frame)) {
            pmm_free_frame(frame);
            vfs_set_cache_page_frame(ent, i, 0);
        }
    }
    obj->cache_page_count = 0;
    if (obj->cache_pages_frame) {
        pmm_free_frame(obj->cache_pages_frame);
        obj->cache_pages_frame = 0;
    }
    vfs_file_sync_entry(ent);
}

static int vfs_file_ensure_capacity(fd_entry_t* ent, unsigned int bytes) {
    unsigned int needed_pages = (bytes + 4095u) / 4096u;
    vfs_file_object_t* obj = vfs_file_object(ent);
    if (!obj) return 0;
    if (needed_pages > PROCESS_FD_CACHE_PAGES) {
        return 0;
    }
    if (needed_pages > 0 && !vfs_file_ensure_page_table(ent)) {
        return 0;
    }
    while (obj->cache_page_count < needed_pages) {
        u32 frame = pmm_alloc_frame();
        if (!frame) {
            return 0;
        }
        vfs_zero_frame(frame);
        if (!vfs_set_cache_page_frame(ent, obj->cache_page_count, frame)) {
            pmm_free_frame(frame);
            return 0;
        }
        obj->cache_page_count++;
    }
    vfs_file_sync_entry(ent);
    return 1;
}

static int vfs_file_load_cache(fd_entry_t* ent) {
    vfs_file_object_t* obj = vfs_file_object(ent);
    if (!obj) return 0;
    if (obj->cache_page_count != 0) return 1;
    if (obj->size == 0) return 1;
    if (obj->size > VFS_FILE_CACHE_MAX_BYTES) return 0;

    if (!vfs_file_ensure_capacity(ent, obj->size)) {
        return 0;
    }

    u32 loaded_size = 0;
    vfs_page_sink_t page_sink = { ent };
    ext2_data_sink_t sink = { &page_sink, vfs_page_sink_write };

    if (!ext2_read_path_to_sink(obj->name, &sink, &loaded_size)) {
        vfs_file_cache_free(ent);
        return 0;
    }
    if (loaded_size != obj->size) {
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
    vfs_file_object_t* obj = vfs_file_object(ent);

    if (!ent || !ent->valid || !obj || !obj->readable) return -EBADF;
    if (obj->is_dir) return -EISDIR;
    if (len == 0) return 0;
    if (obj->offset >= obj->size) return 0;

    if (obj->size <= VFS_FILE_CACHE_MAX_BYTES) {
        unsigned int remaining;
        unsigned int to_copy;
        unsigned int src_off;

        if (!vfs_file_load_cache(ent)) return -EIO;

        remaining = obj->size - obj->offset;
        to_copy = (len < remaining) ? len : remaining;
        src_off = obj->offset;

        if (!vfs_copy_from_cache(ent, src_off, (u8*)buf, to_copy)) return -EIO;

        obj->offset += to_copy;
        vfs_file_sync_entry(ent);
        return (int)to_copy;
    }

    if (!ext2_read_at_path(obj->name, obj->offset, (u8*)buf, len, &read_len)) {
        return -EIO;
    }

    obj->offset += read_len;
    vfs_file_sync_entry(ent);
    return (int)read_len;
}

static int vfs_file_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    vfs_file_object_t* obj = vfs_file_object(ent);
    if (!ent || !ent->valid || !obj || !obj->writable) return -EBADF;
    if (obj->is_dir) return -EISDIR;
    if (len == 0) return 0;

    unsigned int end = obj->offset + len;
    if (end < obj->offset) return -EFBIG;

    if (end <= VFS_FILE_CACHE_MAX_BYTES && obj->size <= VFS_FILE_CACHE_MAX_BYTES) {
        if (obj->size > 0 && obj->cache_page_count == 0 && !vfs_file_load_cache(ent)) {
            return -EIO;
        }
        if (!vfs_file_ensure_capacity(ent, end)) {
            return -EIO;
        }
        if (!vfs_copy_to_cache(ent, obj->offset, (const u8*)buf, len)) {
            return -EIO;
        }

        obj->offset = end;
        if (end > obj->size) {
            obj->size = end;
        }
        obj->dirty = 1;
        vfs_file_sync_entry(ent);
        return (int)len;
    }

    if (obj->dirty && !vfs_file_flush(ent)) {
        return -EIO;
    }
    if (!ext2_write_at_path(obj->name, obj->offset, (const u8*)buf, len, &obj->size, 1)) {
        return -EIO;
    }
    vfs_ro_map_invalidate_path(obj->name);

    obj->offset = end;
    vfs_file_cache_free(ent);
    obj->dirty = 0;
    vfs_file_sync_entry(ent);
    return (int)len;
}

static int vfs_page_source_read(void* ctx, u32 offset, u8* out, u32 len) {
    vfs_page_source_t* source = (vfs_page_source_t*)ctx;
    fd_entry_t* ent = source ? source->ent : 0;
    vfs_file_object_t* obj = vfs_file_object(ent);

    if (!obj || !out) return 0;
    if (offset > obj->size || len > obj->size - offset) return 0;

    return vfs_copy_from_cache(ent, offset, out, len);
}

static int vfs_file_seek(fd_entry_t* ent, int offset, int whence) {
    unsigned int base;
    int new_off;
    vfs_file_object_t* obj = vfs_file_object(ent);

    if (!ent || !ent->valid || !obj) return -EBADF;

    if (whence == 0) {
        base = 0;
    } else if (whence == 1) {
        base = obj->offset;
    } else if (whence == 2) {
        base = obj->size;
    } else {
        return -EINVAL;
    }

    new_off = (int)base + offset;
    if (new_off < 0) return -EINVAL;
    obj->offset = (unsigned int)new_off;
    vfs_file_sync_entry(ent);
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
    vfs_file_object_t* obj = vfs_file_object(ent);
    if (!ent || !ent->valid || !obj || !obj->writable) {
        return 1;
    }
    if (!obj->dirty) {
        return 1;
    }
    if (obj->size > VFS_FILE_CACHE_MAX_BYTES) {
        return 0;
    }

    vfs_page_source_t page_source = { ent };
    ext2_data_source_t source = { &page_source, vfs_page_source_read };

    if (!ext2_write_path_from_source(obj->name, &source, obj->size)) {
        return 0;
    }
    vfs_ro_map_invalidate_path(obj->name);
    obj->dirty = 0;
    vfs_file_sync_entry(ent);
    return 1;
}

static void vfs_file_close(fd_entry_t* ent) {
    vfs_file_object_t* obj;
    u32 obj_frame;

    if (!ent) return;
    obj = vfs_file_object(ent);
    obj_frame = ent->aux_frame;
    if (obj) {
        if (obj->refs > 1u) {
            obj->refs--;
            k_memset(ent, 0, sizeof(*ent));
            return;
        }
        if (obj->writable && obj->dirty) {
            (void)vfs_file_flush(ent);
        }
        vfs_file_cache_free(ent);
        pmm_free_frame(obj_frame);
    }
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

int vfs_file_init(fd_entry_t* ent, const char* path, u32 size, int readable, int writable) {
    vfs_file_object_t* obj;
    u32 frame;

    if (!ent || !path) return -EINVAL;
    frame = pmm_alloc_frame();
    if (!frame) return -ENOMEM;
    obj = (vfs_file_object_t*)paging_phys_to_kernel_virt(frame);
    k_memset(obj, 0, PAGE_SIZE);

    ent->kind = PROCESS_HANDLE_KIND_FILE;
    ent->ops = &s_file_ops;
    ent->readable = readable ? 1 : 0;
    ent->writable = writable ? 1 : 0;
    ent->dirty = 0;
    ent->size = size;
    ent->offset = 0;
    ent->cache_page_count = 0;
    k_memcpy(ent->name, path, (k_size_t)k_strlen(path) + 1u);

    obj->refs = 1u;
    obj->readable = ent->readable;
    obj->writable = ent->writable;
    obj->dirty = 0;
    obj->is_dir = ent->is_dir;
    obj->size = size;
    obj->offset = 0;
    obj->cache_page_count = 0;
    obj->cache_pages_frame = 0;
    k_memcpy(obj->name, path, (k_size_t)k_strlen(path) + 1u);
    ent->aux_frame = frame;
    return 0;
}

void vfs_file_retain(fd_entry_t* ent) {
    vfs_file_object_t* obj = vfs_file_object(ent);
    if (!obj) return;
    obj->refs++;
    vfs_file_sync_entry(ent);
}

void vfs_file_set_is_dir(fd_entry_t* ent, int is_dir) {
    vfs_file_object_t* obj = vfs_file_object(ent);
    if (!obj) {
        if (ent) ent->is_dir = is_dir ? 1 : 0;
        return;
    }
    obj->is_dir = is_dir ? 1 : 0;
    vfs_file_sync_entry(ent);
}

int vfs_file_stat_fd(fd_entry_t* ent, u32* out_size, int* out_is_dir) {
    vfs_file_object_t* obj = vfs_file_object(ent);
    if (!ent || !ent->valid || ent->kind != PROCESS_HANDLE_KIND_FILE || !obj) {
        return -EBADF;
    }
    if (out_size) *out_size = obj->is_dir ? 0u : obj->size;
    if (out_is_dir) *out_is_dir = obj->is_dir ? 1 : 0;
    return 0;
}

static void vfs_copy_ext2_stat(sys_stat_info_t* out, const ext2_stat_info_t* in) {
    if (!out || !in) return;
    k_memset(out, 0, sizeof(*out));
    out->dev = 1;
    out->ino = in->ino;
    out->mode = in->mode;
    out->nlink = in->links_count;
    out->uid = in->uid;
    out->gid = in->gid;
    out->size = in->size;
    out->blksize = 4096u;
    out->blocks = in->blocks_512;
    out->atime = in->atime;
    out->mtime = in->mtime;
    out->ctime = in->ctime;
    out->is_dir = in->is_dir ? 1u : 0u;
}

int vfs_file_stat_info_fd(fd_entry_t* ent, sys_stat_info_t* out) {
    vfs_file_object_t* obj = vfs_file_object(ent);
    ext2_stat_info_t info;

    if (!ent || !ent->valid || ent->kind != PROCESS_HANDLE_KIND_FILE || !obj || !out) {
        return -EBADF;
    }
    if (ext2_stat_info(obj->name, &info)) {
        vfs_copy_ext2_stat(out, &info);
        return 0;
    }

    k_memset(out, 0, sizeof(*out));
    out->mode = (obj->is_dir ? 0040000u : 0100000u) | (obj->is_dir ? 0755u : 0644u);
    out->nlink = obj->is_dir ? 2u : 1u;
    out->size = obj->is_dir ? 0u : obj->size;
    out->blksize = 4096u;
    out->blocks = (out->size + 511u) / 512u;
    out->is_dir = obj->is_dir ? 1u : 0u;
    return 0;
}

int vfs_file_map_ro_page(fd_entry_t* ent,
                         u32 file_offset,
                         u32* out_frame,
                         u32* out_bytes) {
    vfs_file_object_t* obj = vfs_file_object(ent);
    sys_stat_info_t st;
    vfs_ro_map_entry_t* cached;
    vfs_ro_map_entry_t* slot;
    u32 frame;
    u32 bytes;
    unsigned int flags;

    if (out_frame) *out_frame = 0;
    if (out_bytes) *out_bytes = 0;
    if (!ent || !ent->valid || !obj || !out_frame || !out_bytes) return -EBADF;
    if (!obj->readable || obj->writable || obj->is_dir) return -EBADF;
    if ((file_offset & (PAGE_SIZE - 1u)) != 0u) return -EINVAL;
    if (vfs_file_stat_info_fd(ent, &st) < 0) return -EIO;
    if (st.is_dir || file_offset >= st.size) return -EINVAL;

    bytes = st.size - file_offset;
    if (bytes > PAGE_SIZE) bytes = PAGE_SIZE;

    flags = vfs_irq_save();
    cached = vfs_ro_map_find(obj->name, &st, file_offset);
    if (cached) {
        if (!pmm_retain_frame(cached->frame)) {
            vfs_irq_restore(flags);
            return -EIO;
        }
        *out_frame = cached->frame;
        *out_bytes = bytes;
        vfs_irq_restore(flags);
        return 0;
    }
    vfs_irq_restore(flags);

    frame = pmm_alloc_frame();
    if (!frame) return -ENOMEM;
    vfs_zero_frame(frame);
    if (!vfs_file_load_cache(ent) ||
        !vfs_copy_from_cache(ent,
                             file_offset,
                             (u8*)paging_phys_to_kernel_virt(frame),
                             bytes)) {
        pmm_release_frame(frame);
        return -EIO;
    }

    flags = vfs_irq_save();
    cached = vfs_ro_map_find(obj->name, &st, file_offset);
    if (cached) {
        pmm_release_frame(frame);
        if (!pmm_retain_frame(cached->frame)) {
            vfs_irq_restore(flags);
            return -EIO;
        }
        *out_frame = cached->frame;
        *out_bytes = bytes;
        vfs_irq_restore(flags);
        return 0;
    }
    slot = vfs_ro_map_victim();
    if (!slot) {
        pmm_release_frame(frame);
        vfs_irq_restore(flags);
        return -ENOMEM;
    }

    k_memset(slot, 0, sizeof(*slot));
    slot->used = 1;
    k_strncpy(slot->name, obj->name, sizeof(slot->name));
    slot->ino = st.ino;
    slot->size = st.size;
    slot->mtime = st.mtime;
    slot->ctime = st.ctime;
    slot->file_offset = file_offset;
    slot->frame = frame;

    if (!pmm_retain_frame(frame)) {
        vfs_ro_map_drop_entry(slot);
        vfs_irq_restore(flags);
        return -EIO;
    }

    *out_frame = frame;
    *out_bytes = bytes;
    vfs_irq_restore(flags);
    return 0;
}

void vfs_file_map_cache_stats(u32* out_pages, u32* out_mapped_refs) {
    u32 pages = 0;
    u32 refs = 0;
    unsigned int flags = vfs_irq_save();

    for (unsigned int i = 0; i < VFS_RO_MAP_CACHE_PAGES; i++) {
        if (!s_ro_map_cache[i].used || !s_ro_map_cache[i].frame) continue;
        pages++;
        {
            u32 refcount = pmm_frame_refcount(s_ro_map_cache[i].frame);
            if (refcount > 1u) refs += refcount - 1u;
        }
    }

    vfs_irq_restore(flags);
    if (out_pages) *out_pages = pages;
    if (out_mapped_refs) *out_mapped_refs = refs;
}

const u8* vfs_load_file(const char* path, u32* out_size) {
    return ext2_load(path, out_size);
}

static unsigned int vfs_irq_save(void) {
    unsigned int flags;

    __asm__ __volatile__("pushf; pop %0; cli" : "=r"(flags) :: "memory");
    return flags;
}

static void vfs_irq_restore(unsigned int flags) {
    __asm__ __volatile__("push %0; popf" :: "r"(flags) : "memory", "cc");
}

u8* vfs_load_file_owned(const char* path,
                        u32* out_size,
                        u32* out_frame,
                        u32* out_frames) {
    u32 size = 0;
    u32 frames;
    u32 frame;
    u32 read = 0;
    u8* data;
    unsigned int flags;

    if (out_size) *out_size = 0;
    if (out_frame) *out_frame = 0;
    if (out_frames) *out_frames = 0;
    if (!path || !out_size || !out_frame || !out_frames) {
        return 0;
    }
    /*
     * The ext2 driver still owns shared scratch buffers internally. Keep the
     * executable size lookup and read non-preemptible so another task cannot
     * reuse those buffers halfway through loading an image.
     */
    flags = vfs_irq_save();
    if (!ext2_stat(path, &size)) {
        vfs_irq_restore(flags);
        return 0;
    }
    vfs_irq_restore(flags);
    if (size == 0 || size > EXT2_MAX_LOAD_FILE_BYTES) {
        return 0;
    }

    frames = (size + PAGE_SIZE - 1u) / PAGE_SIZE;
    frame = pmm_alloc_contiguous_frames(frames);
    if (!frame) {
        return 0;
    }
    data = (u8*)paging_phys_to_kernel_virt(frame);
    k_memset(data, 0, frames * PAGE_SIZE);

    flags = vfs_irq_save();
    if (!ext2_read_at_path(path, 0, data, size, &read) || read != size) {
        vfs_irq_restore(flags);
        pmm_free_contiguous_frames(frame, frames);
        return 0;
    }
    vfs_irq_restore(flags);

    *out_size = size;
    *out_frame = frame;
    *out_frames = frames;
    return data;
}

void vfs_free_file_owned(u32 frame, u32 frames) {
    if (frame && frames) {
        pmm_free_contiguous_frames(frame, frames);
    }
}

int vfs_stat(const char* path, u32* out_size, int* out_is_dir) {
    u32 size = 0;

    if (ext2_stat(path, &size)) {
        if (out_size) {
            *out_size = size;
        }
        if (out_is_dir) {
            *out_is_dir = 0;
        }
        return 1;
    }

    if (!ext2_is_dir(path)) {
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

int vfs_stat_info(const char* path, sys_stat_info_t* out) {
    ext2_stat_info_t info;

    if (!out) return 0;
    if (!ext2_stat_info(path, &info)) return 0;
    vfs_copy_ext2_stat(out, &info);
    return 1;
}

int vfs_is_dir(const char* path) {
    return ext2_is_dir(path);
}

int vfs_write_root(const char* name, const u8* data, u32 size) {
    vfs_ro_map_invalidate_path(name);
    return ext2_write(name, data, size);
}

int vfs_write_path(const char* path, const u8* data, u32 size) {
    int ok = ext2_write_path(path, data, size);
    if (ok) vfs_ro_map_invalidate_path(path);
    return ok;
}

int vfs_unlink(const char* path) {
    int ok = ext2_rm(path);
    if (ok) vfs_ro_map_invalidate_path(path);
    return ok;
}

int vfs_rename(const char* src, const char* dst) {
    int ok = ext2_move(src, dst);
    if (ok) {
        vfs_ro_map_invalidate_path(src);
        vfs_ro_map_invalidate_path(dst);
    }
    return ok;
}

int vfs_mkdir(const char* path) {
    return ext2_mkdir(path);
}

int vfs_rmdir(const char* path) {
    return ext2_rmdir(path);
}

int vfs_dirent_at(const char* path,
                  u32 index,
                  char* out_name,
                  u32 out_name_size,
                  u32* out_size,
                  int* out_is_dir) {
    return ext2_dirent_at(path, index, out_name, out_name_size,
                          out_size, out_is_dir);
}

int vfs_dirents_read(const char* path,
                     u32 start_index,
                     ext2_dirent_info_t* out,
                     u32 max_entries,
                     u32* out_count) {
    return ext2_dirents_read(path, start_index, out, max_entries, out_count);
}
