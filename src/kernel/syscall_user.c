#include "syscall_internal.h"
#include "klib.h"
#include "paging.h"
#include "process.h"
#include "scheduler.h"
#include "uapi_errno.h"
#include "vfs.h"

int path_is_sep(char c) {
    return c == '/' || c == '\\';
}

static int path_add_component(char comps[][32], int* count, const char* component) {
    if (*count >= 16) {
        return 0;
    }

    int len = 0;
    while (component[len] != '\0') {
        if (len >= 31) {
            return 0;
        }
        len++;
    }

    for (int i = 0; i < len; i++) {
        comps[*count][i] = component[i];
    }
    comps[*count][len] = '\0';
    (*count)++;
    return 1;
}

int path_build_from(const char* base, const char* path, char* out, unsigned int out_size) {
    char comps[16][32];
    const char* sources[2];
    int source_count = 0;
    int count = 0;

    if (!path || !out || out_size == 0) {
        return 0;
    }

    if (base && base[0] != '\0' && !path_is_sep(path[0])) {
        sources[source_count++] = base;
    }
    sources[source_count++] = path;

    for (int s = 0; s < source_count; s++) {
        const char* cursor = sources[s];
        while (*cursor) {
            while (*cursor && path_is_sep(*cursor)) {
                cursor++;
            }
            if (*cursor == '\0') {
                break;
            }

            char component[32];
            int len = 0;
            while (cursor[len] && !path_is_sep(cursor[len])) {
                if (len >= 31) {
                    return 0;
                }
                component[len] = cursor[len];
                len++;
            }
            component[len] = '\0';
            cursor += len;

            if (k_strcmp(component, ".")) {
                continue;
            }
            if (k_strcmp(component, "..")) {
                if (count > 0) {
                    count--;
                }
                continue;
            }
            if (!path_add_component(comps, &count, component)) {
                return 0;
            }
        }
    }

    if (count == 0) {
        out[0] = '\0';
        return 1;
    }

    unsigned int pos = 0;
    for (int i = 0; i < count; i++) {
        unsigned int len = 0;
        while (comps[i][len] != '\0') {
            len++;
        }

        if (pos + len + (i > 0 ? 1u : 0u) + 1u > out_size) {
            return 0;
        }
        if (i > 0) {
            out[pos++] = '/';
        }
        for (unsigned int j = 0; j < len; j++) {
            out[pos++] = comps[i][j];
        }
    }
    out[pos] = '\0';
    return 1;
}

/* ------------------------------------------------------------------ */
/* Copy-from-user validation                                          */
/* ------------------------------------------------------------------ */

static process_t* current_user_proc(void) {
    return (process_t*)sched_current();
}

/*
 * user_page_mapped(pd, addr)
 *
 * Return 1 if the 4 KB page containing addr is present and user-accessible
 * in the given page directory.
 */
int user_page_mapped(u32* pd, unsigned int addr) {
    u32* pd_virt = (u32*)paging_phys_to_kernel_virt((u32)pd);
    u32 pde = pd_virt[addr >> 22];
    if (!(pde & PAGE_PRESENT)) return 0;
    if (!(pde & PAGE_USER))    return 0;

    u32* pt = (u32*)paging_phys_to_kernel_virt(pde & ~0xFFFu);
    u32 pte = pt[(addr >> 12) & 0x3FF];
    if (!(pte & PAGE_PRESENT)) return 0;
    if (!(pte & PAGE_USER))    return 0;
    return 1;
}

static int user_page_access_ok(process_t* proc, unsigned int addr, int write) {
    if (!proc || !proc->pd) return 0;
    if (addr < USER_CODE_BASE || addr >= USER_STACK_TOP) return 0;
    if (user_page_mapped(proc->pd, addr)) {
        if (!write) return 1;
        return process_vm_fault_in_page(proc, addr, 1);
    }
    return process_vm_fault_in_page(proc, addr, write);
}

/*
 * user_buf_ok(ptr, len)
 *
 * Return 1 only if [ptr, ptr + len) lies entirely in mapped user memory.
 * This validates both address range and page-table presence so kernel code
 * never dereferences an unmapped user page by accident.
 */
int user_buf_ok(unsigned int ptr, unsigned int len) {
    if (ptr < USER_CODE_BASE)       return 0;
    if (ptr >= USER_STACK_TOP)      return 0;
    if (len == 0)                   return 0;
    if (len > USER_STACK_TOP - ptr) return 0;

    process_t* proc = current_user_proc();
    if (!proc || !proc->pd) return 0;

    unsigned int start_page = ptr & ~(PAGE_SIZE - 1u);
    unsigned int end_page = (ptr + len - 1u) & ~(PAGE_SIZE - 1u);
    unsigned int page = start_page;

    while (1) {
        if (!user_page_access_ok(proc, page, 0)) return 0;
        if (page == end_page) break;
        page += PAGE_SIZE;
    }

    return 1;
}

int user_buf_write_ok(unsigned int ptr, unsigned int len) {
    if (ptr < USER_CODE_BASE)       return 0;
    if (ptr >= USER_STACK_TOP)      return 0;
    if (len == 0)                   return 0;
    if (len > USER_STACK_TOP - ptr) return 0;

    process_t* proc = current_user_proc();
    if (!proc || !proc->pd) return 0;

    unsigned int start_page = ptr & ~(PAGE_SIZE - 1u);
    unsigned int end_page = (ptr + len - 1u) & ~(PAGE_SIZE - 1u);
    unsigned int page = start_page;

    while (1) {
        if (!user_page_access_ok(proc, page, 1)) return 0;
        if (page == end_page) break;
        page += PAGE_SIZE;
    }

    return 1;
}

int user_count_bytes_ok(unsigned int ptr,
                               unsigned int count,
                               unsigned int elem_size,
                               unsigned int* out_bytes) {
    unsigned int bytes;

    if (elem_size == 0u) return 0;
    if (count > 0xFFFFFFFFu / elem_size) return 0;
    bytes = count * elem_size;
    if (out_bytes) *out_bytes = bytes;
    return user_buf_ok(ptr, bytes);
}

int copy_from_user(void* dst, const void* src, unsigned int len) {
    if (len == 0u) return 0;
    if (!dst || !src) return -EFAULT;
    if (!user_buf_ok((unsigned int)src, len)) return -EFAULT;
    k_memcpy(dst, src, len);
    return 0;
}

int copy_to_user(void* dst, const void* src, unsigned int len) {
    if (len == 0u) return 0;
    if (!dst || !src) return -EFAULT;
    if (!user_buf_write_ok((unsigned int)dst, len)) return -EFAULT;
    k_memcpy(dst, src, len);
    return 0;
}

int read_user_u32(unsigned int* out, const unsigned int* src) {
    return copy_from_user(out, src, sizeof(*out));
}

int write_user_u32(unsigned int* dst, unsigned int value) {
    return copy_to_user(dst, &value, sizeof(value));
}

/*
 * copy_user_cstr(dst, dst_size, src)
 *
 * Copy a NUL-terminated string from user space into a kernel buffer.
 * The copy stops at the first '\0'.  Returns the number of bytes copied,
 * including the terminator, or -1 on validation failure or truncation.
 */
int copy_user_cstr(char* dst, unsigned int dst_size, const char* src) {
    if (!dst || !src || dst_size == 0) return -EFAULT;

    unsigned int ptr = (unsigned int)src;
    if (ptr < USER_CODE_BASE || ptr >= USER_STACK_TOP) return -EFAULT;

    process_t* proc = current_user_proc();
    if (!proc || !proc->pd) return -EFAULT;

    for (unsigned int i = 0; i < dst_size; i++) {
        unsigned int addr = ptr + i;
        if (addr < USER_CODE_BASE || addr >= USER_STACK_TOP) return -EFAULT;
        if (!user_page_access_ok(proc, addr, 0)) return -EFAULT;

        dst[i] = src[i];
        if (dst[i] == '\0') {
            return (int)(i + 1);
        }
    }

    return -ENAMETOOLONG;
}

int copy_user_path_resolved(char* dst, unsigned int dst_size, const char* src) {
    char raw[PROCESS_FD_NAME_MAX];
    process_t* proc = (process_t*)sched_current();

    int copied = copy_user_cstr(raw, sizeof(raw), src);
    if (copied < 0) return copied;
    if (copied <= 1) return -EINVAL;
    if (!proc) return -EINVAL;
    if (!path_build_from(proc->cwd, raw, dst, dst_size)) return -ENAMETOOLONG;
    return 1;
}

int copy_user_path_at_resolved(char* dst,
                                      unsigned int dst_size,
                                      int dirfd,
                                      const char* src) {
    char raw[PROCESS_FD_NAME_MAX];
    const char* base = 0;
    process_t* proc = (process_t*)sched_current();
    int copied = copy_user_cstr(raw, sizeof(raw), src);

    if (copied < 0) return copied;
    if (copied <= 1) return -EINVAL;
    if (!proc) return -EINVAL;

    if (path_is_sep(raw[0])) {
        base = 0;
    } else if (dirfd == SYS_AT_FDCWD) {
        base = proc->cwd;
    } else {
        fd_entry_t* ent = process_fd_get(proc, dirfd);
        if (!ent) return -EBADF;
        if (ent->kind != PROCESS_HANDLE_KIND_FILE || !ent->is_dir) return -ENOTDIR;
        base = ent->name;
    }

    if (!path_build_from(base, raw, dst, dst_size)) return -ENAMETOOLONG;
    return 1;
}

int path_lookup_errno(const char* path) {
    char prefix[PROCESS_FD_NAME_MAX];
    u32 size = 0;
    int is_dir = 0;

    if (!path || path[0] == '\0') {
        return vfs_is_dir("") ? 0 : -ENOENT;
    }

    for (unsigned int i = 0; path[i] != '\0'; i++) {
        if (path[i] != '/') {
            continue;
        }
        if (i == 0 || i >= sizeof(prefix)) {
            continue;
        }
        k_memcpy(prefix, path, i);
        prefix[i] = '\0';
        if (vfs_stat(prefix, &size, &is_dir) && !is_dir) {
            return -ENOTDIR;
        }
    }

    if (vfs_stat(path, &size, &is_dir) || vfs_is_dir(path)) {
        return 0;
    }
    return -ENOENT;
}
