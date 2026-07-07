#include "syscall.h"
#include "terminal.h"
#include "timer.h"
#include "paging.h"
#include "keyboard.h"
#include "scheduler.h"
#include "process.h"
#include "pmm.h"
#include "memory.h"
#include "boot_info.h"
#include "system.h"
#include "klib.h"
#include "../drivers/nic.h"
#include "../drivers/net.h"
#include "../drivers/dhcp.h"
#include "../drivers/arp.h"
#include "../drivers/ipv4.h"
#include "../drivers/tcp.h"
#include "../drivers/ntp.h"
#include "../drivers/mouse.h"
#include "../drivers/usb.h"
#include "../drivers/sound.h"
#include "uapi_poll.h"
#include "uapi_errno.h"
#include "uapi_dirent.h"
#include "uapi_socket.h"
#include "uapi_net.h"
#include "uapi_netlink.h"
#include "uapi_epoll.h"
#include "uapi_display.h"
#include "uapi_input.h"
#include "uapi_sound.h"
#include "uapi_syscall.h"
#include "../exec/elf_loader.h"
#include "vfs.h"
#include "socket.h"
#include "input.h"
#include "wait.h"
#include "../drivers/display.h"
#include "../drivers/ext2.h"
#include "gdt.h"

#define SYSCALL_MAX_WRITE_LEN 4096u
#define EXEC_NAME_MAX         PROCESS_FD_NAME_MAX
#define EPOLL_MAX_WATCHES     64u
#define POLL_MAX_FDS          PROCESS_FD_LIMIT_HARD
#define INPUT_READ_MAX_EVENTS 64u
#define DIRLIST_BATCH_MAX     64u
#define SYS_PROT_READ         1u
#define SYS_PROT_WRITE        2u
#define SYS_PROT_EXEC         4u
#define SYS_MAP_PRIVATE       0x02u
#define SYS_MAP_FIXED         0x10u
#define SYS_MAP_ANON          0x20u
#define SYS_AT_FDCWD          (-100)
#define SYS_AT_SYMLINK_NOFOLLOW 0x100u
#define SYS_AT_REMOVEDIR      0x200u
#define SYS_AT_SYMLINK_FOLLOW 0x400u
#define SYS_IOCTL_TIOCGPGRP   0x540Fu
#define SYS_IOCTL_TIOCSPGRP   0x5410u
#define SYS_IOCTL_TIOCGWINSZ  0x5413u
#define SYS_RLIMIT_CPU        0
#define SYS_RLIMIT_FSIZE      1
#define SYS_RLIMIT_DATA       2
#define SYS_RLIMIT_STACK      3
#define SYS_RLIMIT_CORE       4
#define SYS_RLIMIT_AS         6
#define SYS_RLIMIT_NOFILE     7
#define SYS_RLIM_INFINITY     0xFFFFFFFFu
#define SYS_RUSAGE_SELF       0
#define SYS_RUSAGE_CHILDREN  (-1)
#define SYS_MOUNT_MS_RDONLY      1u
#define SYS_MOUNT_MS_NOSUID      2u
#define SYS_MOUNT_MS_NODEV       4u
#define SYS_MOUNT_MS_NOEXEC      8u
#define SYS_MOUNT_MS_SYNCHRONOUS 16u
#define SYS_MOUNT_MS_REMOUNT     32u
#define SYS_MOUNT_MS_MANDLOCK    64u
#define SYS_MOUNT_MS_DIRSYNC     128u
#define SYS_MOUNT_MS_NOSYMFOLLOW 256u
#define SYS_NETLINK_MAX_PACKET 4096u
#define SYS_MOUNT_MS_NOATIME     1024u
#define SYS_MOUNT_MS_NODIRATIME  2048u
#define SYS_MOUNT_MS_BIND        4096u
#define SYS_MOUNT_MS_MOVE        8192u
#define SYS_MOUNT_MS_REC         16384u
#define SYS_MOUNT_MS_SILENT      32768u
#define SYS_MOUNT_MS_UNBINDABLE  (1u << 17)
#define SYS_MOUNT_MS_PRIVATE     (1u << 18)
#define SYS_MOUNT_MS_SLAVE       (1u << 19)
#define SYS_MOUNT_MS_SHARED      (1u << 20)
#define SYS_MOUNT_MS_RELATIME    (1u << 21)
#define SYS_MOUNT_MS_STRICTATIME (1u << 24)
#define SYS_MOUNT_MS_LAZYTIME    (1u << 25)
#define SYS_MOUNT_MS_MGC_VAL     0xc0ed0000u
#define SYS_MOUNT_MS_MGC_MSK     0xffff0000u
#define SYS_MOUNT_SUPPORTED_FLAGS \
    (SYS_MOUNT_MS_RDONLY | SYS_MOUNT_MS_NOSUID | SYS_MOUNT_MS_NODEV | \
     SYS_MOUNT_MS_NOEXEC | SYS_MOUNT_MS_SYNCHRONOUS | SYS_MOUNT_MS_MANDLOCK | \
     SYS_MOUNT_MS_DIRSYNC | SYS_MOUNT_MS_NOSYMFOLLOW | SYS_MOUNT_MS_NOATIME | \
     SYS_MOUNT_MS_NODIRATIME | SYS_MOUNT_MS_SILENT | SYS_MOUNT_MS_RELATIME | \
     SYS_MOUNT_MS_STRICTATIME | SYS_MOUNT_MS_LAZYTIME)
#define SYS_MOUNT_ACTION_FLAGS \
    (SYS_MOUNT_MS_REMOUNT | SYS_MOUNT_MS_BIND | SYS_MOUNT_MS_MOVE | \
     SYS_MOUNT_MS_REC | SYS_MOUNT_MS_UNBINDABLE | SYS_MOUNT_MS_PRIVATE | \
     SYS_MOUNT_MS_SLAVE | SYS_MOUNT_MS_SHARED)
#define SYS_UMOUNT_MNT_FORCE  1u
#define SYS_UMOUNT_MNT_DETACH 2u
#define SYS_STATFS_EXT2_MAGIC 0xEF53L
#define SYS_STATFS_PROC_MAGIC 0x9FA0L
#define SYS_STATFS_DEV_MAGIC  0x01021994L
#define SYS_MOUNT_MAX         16u

static unsigned char s_sys_block_sector[512] __attribute__((aligned(16)));
static volatile int s_sys_block_sector_locked = 0;

typedef struct epoll_watch {
    int used;
    int fd;
    unsigned int events;
    unsigned int data_u32;
} epoll_watch_t;

typedef struct kernel_mount {
    unsigned int used;
    unsigned int dynamic;
    char source[PROCESS_FD_NAME_MAX];
    char target[PROCESS_FD_NAME_MAX];
    char fstype[16];
    char options[16];
    unsigned int flags;
    long magic;
    unsigned int pseudo;
} kernel_mount_t;

static kernel_mount_t s_mounts[SYS_MOUNT_MAX] = {
    { 1u, 0u, "rootfs", "",     "ext2",     "rw", 0u, SYS_STATFS_EXT2_MAGIC, 0u },
    { 1u, 0u, "proc",   "proc", "proc",     "rw", 0u, SYS_STATFS_PROC_MAGIC, 1u },
    { 1u, 0u, "dev",    "dev",  "devtmpfs", "rw", 0u, SYS_STATFS_DEV_MAGIC,  1u },
};

#define SYS_MOUNT_STATIC_COUNT 3u

struct user_itimerspec {
    struct {
        unsigned int tv_sec;
        long tv_nsec;
    } it_interval;
    struct {
        unsigned int tv_sec;
        long tv_nsec;
    } it_value;
};

struct user_timespec {
    unsigned int tv_sec;
    long tv_nsec;
};

typedef struct syscall_iret_frame {
    unsigned int eip;
    unsigned int cs;
    unsigned int eflags;
    unsigned int user_esp;
    unsigned int ss;
} syscall_iret_frame_t;

static int sys_close_impl(int fd);
static int sys_utimens_kpath_impl(const char* kpath,
                                  const struct user_timespec* times,
                                  int nofollow);
static unsigned int process_ram_bytes(process_t* proc);
static unsigned int net_route_next_hop(unsigned int target_ip);
static int sys_netlink_send_user(fd_entry_t* ent,
                                 const void* buf,
                                 unsigned int len);
static int sys_netlink_recv_user(process_t* proc,
                                 fd_entry_t* ent,
                                 void* buf,
                                 unsigned int len,
                                 unsigned int flags,
                                 struct sockaddr* src_addr,
                                 unsigned int* addrlen);

static int path_is_sep(char c) {
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

static int path_build_from(const char* base, const char* path, char* out, unsigned int out_size) {
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

static u32* current_user_pd(void) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return 0;
    return proc->pd;
}

/*
 * user_page_mapped(pd, addr)
 *
 * Return 1 if the 4 KB page containing addr is present and user-accessible
 * in the given page directory.
 */
static int user_page_mapped(u32* pd, unsigned int addr) {
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

/*
 * user_buf_ok(ptr, len)
 *
 * Return 1 only if [ptr, ptr + len) lies entirely in mapped user memory.
 * This validates both address range and page-table presence so kernel code
 * never dereferences an unmapped user page by accident.
 */
static int user_buf_ok(unsigned int ptr, unsigned int len) {
    if (ptr < USER_CODE_BASE)       return 0;
    if (ptr >= USER_STACK_TOP)      return 0;
    if (len == 0)                   return 0;
    if (len > USER_STACK_TOP - ptr) return 0;

    u32* pd = current_user_pd();
    if (!pd) return 0;

    unsigned int start_page = ptr & ~(PAGE_SIZE - 1u);
    unsigned int end_page = (ptr + len - 1u) & ~(PAGE_SIZE - 1u);
    unsigned int page = start_page;

    while (1) {
        if (!user_page_mapped(pd, page)) return 0;
        if (page == end_page) break;
        page += PAGE_SIZE;
    }

    return 1;
}

static int user_count_bytes_ok(unsigned int ptr,
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

static int copy_from_user(void* dst, const void* src, unsigned int len) {
    if (len == 0u) return 0;
    if (!dst || !src) return -EFAULT;
    if (!user_buf_ok((unsigned int)src, len)) return -EFAULT;
    k_memcpy(dst, src, len);
    return 0;
}

static int copy_to_user(void* dst, const void* src, unsigned int len) {
    if (len == 0u) return 0;
    if (!dst || !src) return -EFAULT;
    if (!user_buf_ok((unsigned int)dst, len)) return -EFAULT;
    k_memcpy(dst, src, len);
    return 0;
}

static int read_user_u32(unsigned int* out, const unsigned int* src) {
    return copy_from_user(out, src, sizeof(*out));
}

static int write_user_u32(unsigned int* dst, unsigned int value) {
    return copy_to_user(dst, &value, sizeof(value));
}

/*
 * copy_user_cstr(dst, dst_size, src)
 *
 * Copy a NUL-terminated string from user space into a kernel buffer.
 * The copy stops at the first '\0'.  Returns the number of bytes copied,
 * including the terminator, or -1 on validation failure or truncation.
 */
static int copy_user_cstr(char* dst, unsigned int dst_size, const char* src) {
    if (!dst || !src || dst_size == 0) return -EFAULT;

    unsigned int ptr = (unsigned int)src;
    if (ptr < USER_CODE_BASE || ptr >= USER_STACK_TOP) return -EFAULT;

    u32* pd = current_user_pd();
    if (!pd) return -EFAULT;

    for (unsigned int i = 0; i < dst_size; i++) {
        unsigned int addr = ptr + i;
        if (addr < USER_CODE_BASE || addr >= USER_STACK_TOP) return -EFAULT;
        if (!user_page_mapped(pd, addr)) return -EFAULT;

        dst[i] = src[i];
        if (dst[i] == '\0') {
            return (int)(i + 1);
        }
    }

    return -ENAMETOOLONG;
}

static int copy_user_path_resolved(char* dst, unsigned int dst_size, const char* src) {
    char raw[PROCESS_FD_NAME_MAX];
    process_t* proc = (process_t*)sched_current();

    int copied = copy_user_cstr(raw, sizeof(raw), src);
    if (copied < 0) return copied;
    if (copied <= 1) return -EINVAL;
    if (!proc) return -EINVAL;
    if (!path_build_from(proc->cwd, raw, dst, dst_size)) return -ENAMETOOLONG;
    return 1;
}

static int copy_user_path_at_resolved(char* dst,
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

static int path_lookup_errno(const char* path) {
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

static int k_is_digit(char c) {
    return c >= '0' && c <= '9';
}

static int k_parse_uint_path(const char* s, unsigned int* out, const char** tail) {
    unsigned int value = 0;
    int saw = 0;

    if (!s || !out) return 0;
    while (k_is_digit(*s)) {
        value = value * 10u + (unsigned int)(*s - '0');
        s++;
        saw = 1;
    }
    if (!saw) return 0;
    *out = value;
    if (tail) *tail = s;
    return 1;
}

static void vbuf_putc(char* out, unsigned int cap, unsigned int* pos, char c) {
    if (!out || !pos || cap == 0u) return;
    if (*pos + 1u < cap) {
        out[*pos] = c;
        (*pos)++;
    }
    out[*pos < cap ? *pos : cap - 1u] = '\0';
}

static void vbuf_puts(char* out, unsigned int cap, unsigned int* pos, const char* s) {
    if (!s) return;
    while (*s) {
        vbuf_putc(out, cap, pos, *s++);
    }
}

static void vbuf_put_uint(char* out, unsigned int cap, unsigned int* pos, unsigned int value) {
    char tmp[16];
    unsigned int n = 0;

    if (value == 0u) {
        vbuf_putc(out, cap, pos, '0');
        return;
    }
    while (value && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + (value % 10u));
        value /= 10u;
    }
    while (n) {
        vbuf_putc(out, cap, pos, tmp[--n]);
    }
}

static void vbuf_put_hex8(char* out, unsigned int cap, unsigned int* pos, unsigned int value) {
    static const char hexdigits[] = "0123456789ABCDEF";
    for (int shift = 28; shift >= 0; shift -= 4) {
        vbuf_putc(out, cap, pos, hexdigits[(value >> shift) & 0xFu]);
    }
}

static unsigned int proc_route_hex_ip(unsigned int ip) {
    return ((ip & 0x000000FFu) << 24)
         | ((ip & 0x0000FF00u) << 8)
         | ((ip & 0x00FF0000u) >> 8)
         | ((ip & 0xFF000000u) >> 24);
}

static void vbuf_put_proc_route_ip(char* out,
                                   unsigned int cap,
                                   unsigned int* pos,
                                   unsigned int ip) {
    vbuf_put_hex8(out, cap, pos, proc_route_hex_ip(ip));
}

static void vbuf_put_kb_line(char* out,
                             unsigned int cap,
                             unsigned int* pos,
                             const char* name,
                             unsigned int bytes) {
    vbuf_puts(out, cap, pos, name);
    vbuf_putc(out, cap, pos, ':');
    vbuf_putc(out, cap, pos, '\t');
    vbuf_put_uint(out, cap, pos, bytes / 1024u);
    vbuf_puts(out, cap, pos, " kB\n");
}

static int path_eq(const char* path, const char* literal) {
    return k_strcmp(path, literal);
}

static int path_child(const char* path, const char* prefix, const char** child) {
    unsigned int len;

    if (!path || !prefix || !child) return 0;
    len = (unsigned int)k_strlen(prefix);
    if (!k_starts_with(path, prefix)) return 0;
    if (path[len] != '/') return 0;
    *child = path + len + 1u;
    return 1;
}

static int mount_path_matches(const char* path, const char* target) {
    unsigned int len;

    if (!path || !target) return 0;
    if (target[0] == '\0') return 1;
    len = (unsigned int)k_strlen(target);
    if (!k_starts_with(path, target)) return 0;
    return path[len] == '\0' || path[len] == '/';
}

static int mount_is_proc(const kernel_mount_t* mnt) {
    return mnt && mnt->used && path_eq(mnt->fstype, "proc");
}

static int mount_is_devtmpfs(const kernel_mount_t* mnt) {
    return mnt && mnt->used && path_eq(mnt->fstype, "devtmpfs");
}

static const kernel_mount_t* mount_find_for_path(const char* path) {
    const kernel_mount_t* best = &s_mounts[0];
    unsigned int best_len = 0u;

    if (!path) return best;
    for (unsigned int i = 0; i < SYS_MOUNT_MAX; i++) {
        unsigned int len;
        if (!s_mounts[i].used) continue;
        if (!mount_path_matches(path, s_mounts[i].target)) continue;
        len = (unsigned int)k_strlen(s_mounts[i].target);
        if (!best || len >= best_len) {
            best = &s_mounts[i];
            best_len = len;
        }
    }
    return best;
}

static const kernel_mount_t* mount_find_exact(const char* path) {
    if (!path) return 0;
    for (unsigned int i = 0; i < SYS_MOUNT_MAX; i++) {
        if (!s_mounts[i].used) continue;
        if (path_eq(path, s_mounts[i].target)) return &s_mounts[i];
    }
    return 0;
}

static kernel_mount_t* mount_find_exact_mutable(const char* path) {
    if (!path) return 0;
    for (unsigned int i = 0; i < SYS_MOUNT_MAX; i++) {
        if (!s_mounts[i].used) continue;
        if (path_eq(path, s_mounts[i].target)) return &s_mounts[i];
    }
    return 0;
}

static kernel_mount_t* mount_alloc_slot(void) {
    for (unsigned int i = SYS_MOUNT_STATIC_COUNT; i < SYS_MOUNT_MAX; i++) {
        if (!s_mounts[i].used) return &s_mounts[i];
    }
    return 0;
}

static int mount_translate_virtual_path(const char* path,
                                        char* out,
                                        unsigned int out_size) {
    const kernel_mount_t* mnt;
    const char* base;
    const char* suffix;
    unsigned int pos = 0;
    unsigned int target_len;

    if (!path || !out || out_size == 0u) return 0;
    mnt = mount_find_for_path(path);
    if (!mnt || !mnt->pseudo) return 0;
    if (mount_is_proc(mnt)) {
        base = "proc";
    } else if (mount_is_devtmpfs(mnt)) {
        base = "dev";
    } else {
        return 0;
    }

    target_len = (unsigned int)k_strlen(mnt->target);
    suffix = path + target_len;
    if (mnt->target[0] != '\0' && *suffix != '\0' && *suffix != '/') {
        return 0;
    }

    out[0] = '\0';
    vbuf_puts(out, out_size, &pos, base);
    if (*suffix == '/') {
        vbuf_puts(out, out_size, &pos, suffix);
    }
    return out[0] != '\0';
}

static const char* virtual_effective_path(const char* path,
                                          char* translated,
                                          unsigned int translated_size) {
    if (mount_translate_virtual_path(path, translated, translated_size)) {
        return translated;
    }
    return path;
}

static unsigned int mount_build_proc_content(char* out, unsigned int cap) {
    unsigned int pos = 0;

    if (!out || cap == 0u) return 0;
    out[0] = '\0';
    for (unsigned int i = 0; i < SYS_MOUNT_MAX; i++) {
        if (!s_mounts[i].used) continue;
        vbuf_puts(out, cap, &pos, s_mounts[i].source);
        vbuf_putc(out, cap, &pos, ' ');
        if (s_mounts[i].target[0] == '\0') {
            vbuf_putc(out, cap, &pos, '/');
        } else {
            vbuf_putc(out, cap, &pos, '/');
            vbuf_puts(out, cap, &pos, s_mounts[i].target);
        }
        vbuf_putc(out, cap, &pos, ' ');
        vbuf_puts(out, cap, &pos, s_mounts[i].fstype);
        vbuf_putc(out, cap, &pos, ' ');
        vbuf_puts(out, cap, &pos, s_mounts[i].options);
        vbuf_puts(out, cap, &pos, " 0 0\n");
    }
    return pos;
}

static int virtual_proc_pid_path(const char* path,
                                 process_t** out_proc,
                                 const char** out_leaf) {
    const char* rest;
    const char* tail;
    unsigned int pid;

    if (!path_child(path, "proc", &rest)) return 0;
    if (k_starts_with(rest, "self")) {
        process_t* cur;
        tail = rest + 4;
        if (*tail != '\0' && *tail != '/') return 0;
        cur = (process_t*)sched_current();
        if (!cur) return 0;
        if (out_proc) *out_proc = cur;
        if (out_leaf) *out_leaf = (*tail == '/') ? tail + 1 : "";
        return 1;
    }
    if (!k_parse_uint_path(rest, &pid, &tail)) return 0;
    if (*tail != '\0' && *tail != '/') return 0;
    if (out_proc) *out_proc = process_find_by_pid(pid);
    if (out_leaf) *out_leaf = (*tail == '/') ? tail + 1 : "";
    return 1;
}

static int virtual_path_is_dir(const char* path) {
    process_t* proc = 0;
    const char* leaf = 0;
    char translated[PROCESS_FD_NAME_MAX];

    path = virtual_effective_path(path, translated, sizeof(translated));
    if (path_eq(path, "proc") || path_eq(path, "proc/net") ||
        path_eq(path, "dev") || path_eq(path, "dev/fd")) {
        return 1;
    }
    if (virtual_proc_pid_path(path, &proc, &leaf)) {
        return proc && leaf && leaf[0] == '\0';
    }
    return 0;
}

static int virtual_path_is_dev(const char* path, unsigned int* out_type) {
    char translated[PROCESS_FD_NAME_MAX];

    path = virtual_effective_path(path, translated, sizeof(translated));
    if (path_eq(path, "dev/null")) {
        if (out_type) *out_type = PROCESS_VIRTUAL_NULL;
        return 1;
    }
    if (path_eq(path, "dev/zero")) {
        if (out_type) *out_type = PROCESS_VIRTUAL_ZERO;
        return 1;
    }
    if (path_eq(path, "dev/tty") || path_eq(path, "dev/console") ||
        path_eq(path, "dev/fd/0") || path_eq(path, "dev/fd/1") ||
        path_eq(path, "dev/fd/2")) {
        if (out_type) *out_type = PROCESS_VIRTUAL_TTY;
        return 1;
    }
    return 0;
}

static const char* process_state_name(unsigned int state) {
    switch (state) {
        case PROCESS_STATE_RUNNING: return "R";
        case PROCESS_STATE_EXITED: return "X";
        case PROCESS_STATE_ZOMBIE: return "Z";
        case PROCESS_STATE_WAITING: return "S";
        case PROCESS_STATE_SLEEPING: return "S";
        case PROCESS_STATE_STOPPED: return "T";
        default: return "?";
    }
}

static unsigned int virtual_build_proc_content(const char* path,
                                               char* out,
                                               unsigned int cap) {
    unsigned int pos = 0;
    process_t* proc = 0;
    const char* leaf = 0;
    char translated[PROCESS_FD_NAME_MAX];

    if (!out || cap == 0u) return 0;
    path = virtual_effective_path(path, translated, sizeof(translated));
    out[0] = '\0';

    if (path_eq(path, "proc/meminfo")) {
        unsigned int total = pmm_total_count() * PAGE_SIZE;
        unsigned int free = pmm_free_count() * PAGE_SIZE;
        vbuf_put_kb_line(out, cap, &pos, "MemTotal", total);
        vbuf_put_kb_line(out, cap, &pos, "MemFree", free);
        vbuf_put_kb_line(out, cap, &pos, "MemAvailable", free);
        return pos;
    }
    if (path_eq(path, "proc/uptime")) {
        unsigned int ticks = timer_get_ticks();
        unsigned int sec = ticks / SMALLOS_TIMER_HZ;
        unsigned int frac = ((ticks % SMALLOS_TIMER_HZ) * 100u) / SMALLOS_TIMER_HZ;
        vbuf_put_uint(out, cap, &pos, sec);
        vbuf_putc(out, cap, &pos, '.');
        if (frac < 10u) vbuf_putc(out, cap, &pos, '0');
        vbuf_put_uint(out, cap, &pos, frac);
        vbuf_putc(out, cap, &pos, ' ');
        vbuf_put_uint(out, cap, &pos, sec);
        vbuf_puts(out, cap, &pos, ".00\n");
        return pos;
    }
    if (path_eq(path, "proc/stat")) {
        unsigned int ticks = timer_get_ticks();
        vbuf_puts(out, cap, &pos, "cpu  ");
        vbuf_put_uint(out, cap, &pos, ticks);
        vbuf_puts(out, cap, &pos, " 0 0 0 0 0 0 0 0 0\n");
        vbuf_puts(out, cap, &pos, "intr 0\nctxt 0\nbtime 0\nprocesses ");
        vbuf_put_uint(out, cap, &pos, ticks);
        vbuf_putc(out, cap, &pos, '\n');
        return pos;
    }
    if (path_eq(path, "proc/mounts")) {
        return mount_build_proc_content(out, cap);
    }
    if (path_eq(path, "proc/filesystems")) {
        vbuf_puts(out, cap, &pos, "nodev\tproc\nnodev\tdevtmpfs\next2\n");
        return pos;
    }
    if (path_eq(path, "proc/net/dev")) {
        nic_stats_t stats;
        const net_ipv4_config_t* cfg = net_ipv4_config();
        nic_get_stats(&stats);
        vbuf_puts(out, cap, &pos, "Inter-|   Receive                                                |  Transmit\n");
        vbuf_puts(out, cap, &pos, " face |bytes    packets errs drop fifo frame compressed multicast|bytes    packets errs drop fifo colls carrier compressed\n");
        vbuf_puts(out, cap, &pos, "  eth0:");
        vbuf_put_uint(out, cap, &pos, stats.rx_packets * 64u);
        vbuf_putc(out, cap, &pos, ' ');
        vbuf_put_uint(out, cap, &pos, stats.rx_packets);
        vbuf_putc(out, cap, &pos, ' ');
        vbuf_put_uint(out, cap, &pos, stats.rx_errors);
        vbuf_puts(out, cap, &pos, " 0 0 0 0 0 ");
        vbuf_put_uint(out, cap, &pos, stats.tx_packets * 64u);
        vbuf_putc(out, cap, &pos, ' ');
        vbuf_put_uint(out, cap, &pos, stats.tx_packets);
        vbuf_putc(out, cap, &pos, ' ');
        vbuf_put_uint(out, cap, &pos, stats.tx_errors);
        vbuf_puts(out, cap, &pos, " 0 0 0 0 0\n");
        (void)cfg;
        return pos;
    }
    if (path_eq(path, "proc/net/route")) {
        const net_ipv4_config_t* cfg = net_ipv4_config();
        vbuf_puts(out, cap, &pos, "Iface\tDestination\tGateway \tFlags\tRefCnt\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n");
        if (cfg && cfg->configured) {
            if (cfg->gateway != 0u) {
                vbuf_puts(out, cap, &pos, "eth0\t");
                vbuf_put_proc_route_ip(out, cap, &pos, 0u);
                vbuf_putc(out, cap, &pos, '\t');
                vbuf_put_proc_route_ip(out, cap, &pos, cfg->gateway);
                vbuf_puts(out, cap, &pos, "\t0003\t0\t0\t0\t");
                vbuf_put_proc_route_ip(out, cap, &pos, 0u);
                vbuf_puts(out, cap, &pos, "\t0\t0\t0\n");
            }
            vbuf_puts(out, cap, &pos, "eth0\t");
            vbuf_put_proc_route_ip(out, cap, &pos, cfg->ip & cfg->netmask);
            vbuf_puts(out, cap, &pos, "\t00000000\t0001\t0\t0\t0\t");
            vbuf_put_proc_route_ip(out, cap, &pos, cfg->netmask);
            vbuf_puts(out, cap, &pos, "\t0\t0\t0\n");
        }
        return pos;
    }

    if (!virtual_proc_pid_path(path, &proc, &leaf) || !proc || !leaf) {
        return 0;
    }
    if (path_eq(leaf, "comm")) {
        vbuf_puts(out, cap, &pos, proc->name);
        vbuf_putc(out, cap, &pos, '\n');
        return pos;
    }
    if (path_eq(leaf, "cmdline")) {
        for (int i = 0; i < proc->user_argc; i++) {
            if (i) vbuf_putc(out, cap, &pos, '\0');
            vbuf_puts(out, cap, &pos, proc->user_argv[i]);
        }
        if (pos == 0u) vbuf_puts(out, cap, &pos, proc->name);
        return pos;
    }
    if (path_eq(leaf, "status")) {
        vbuf_puts(out, cap, &pos, "Name:\t");
        vbuf_puts(out, cap, &pos, proc->name);
        vbuf_puts(out, cap, &pos, "\nState:\t");
        vbuf_puts(out, cap, &pos, process_state_name((unsigned int)proc->state));
        vbuf_puts(out, cap, &pos, "\nPid:\t");
        vbuf_put_uint(out, cap, &pos, proc->pid);
        vbuf_puts(out, cap, &pos, "\nPPid:\t");
        vbuf_put_uint(out, cap, &pos, proc->parent_pid);
        vbuf_puts(out, cap, &pos, "\nUid:\t0\t0\t0\t0\nGid:\t0\t0\t0\t0\n");
        return pos;
    }
    if (path_eq(leaf, "stat")) {
        vbuf_put_uint(out, cap, &pos, proc->pid);
        vbuf_puts(out, cap, &pos, " (");
        vbuf_puts(out, cap, &pos, proc->name);
        vbuf_puts(out, cap, &pos, ") ");
        vbuf_puts(out, cap, &pos, process_state_name((unsigned int)proc->state));
        vbuf_putc(out, cap, &pos, ' ');
        vbuf_put_uint(out, cap, &pos, proc->parent_pid);
        vbuf_putc(out, cap, &pos, ' ');
        vbuf_put_uint(out, cap, &pos, proc->pgid);
        vbuf_puts(out, cap, &pos, " 0 0 0 0 0 0 0 0 0 0 ");
        vbuf_put_uint(out, cap, &pos, proc->cpu_ticks);
        vbuf_puts(out, cap, &pos, " 0 0 0 20 0 1 0 0 ");
        vbuf_put_uint(out, cap, &pos, proc->heap_brk >= proc->heap_base
                                      ? proc->heap_brk - proc->heap_base
                                      : 0u);
        vbuf_putc(out, cap, &pos, '\n');
        return pos;
    }

    return 0;
}

static int virtual_path_regular_size(const char* path, unsigned int* out_size) {
    char tmp[PAGE_SIZE];
    unsigned int size;
    char translated[PROCESS_FD_NAME_MAX];

    if (!path || !out_size) return 0;
    path = virtual_effective_path(path, translated, sizeof(translated));
    if (!k_starts_with(path, "proc/")) return 0;
    size = virtual_build_proc_content(path, tmp, sizeof(tmp));
    if (size == 0u) return 0;
    *out_size = size;
    return 1;
}

static int virtual_path_exists(const char* path,
                               unsigned int* out_size,
                               int* out_is_dir,
                               unsigned int* out_type) {
    unsigned int size = 0;
    unsigned int type = PROCESS_VIRTUAL_REGULAR;
    int is_dir = 0;

    if (!path) return 0;
    if (virtual_path_is_dir(path)) {
        is_dir = 1;
    } else if (virtual_path_is_dev(path, &type)) {
        size = 0;
    } else if (!virtual_path_regular_size(path, &size)) {
        return 0;
    }
    if (out_size) *out_size = size;
    if (out_is_dir) *out_is_dir = is_dir;
    if (out_type) *out_type = type;
    return 1;
}

static int virtual_stat_info(const char* path, sys_stat_info_t* info) {
    unsigned int size = 0;
    unsigned int type = PROCESS_VIRTUAL_REGULAR;
    int is_dir = 0;
    unsigned int mode;

    if (!info || !virtual_path_exists(path, &size, &is_dir, &type)) return 0;
    k_memset(info, 0, sizeof(*info));
    mode = is_dir ? (0040000u | 0555u) : (0100000u | 0444u);
    if (type == PROCESS_VIRTUAL_NULL ||
        type == PROCESS_VIRTUAL_ZERO ||
        type == PROCESS_VIRTUAL_TTY) {
        mode = 0020000u | 0666u;
        info->rdev = type;
    }
    info->dev = 2u;
    info->ino = 2000u;
    for (unsigned int i = 0; path[i]; i++) {
        info->ino = info->ino * 33u + (unsigned char)path[i];
    }
    info->mode = mode;
    info->nlink = is_dir ? 2u : 1u;
    info->uid = 0u;
    info->gid = 0u;
    info->size = size;
    info->blksize = PAGE_SIZE;
    info->blocks = (size + 511u) / 512u;
    info->atime = timer_get_seconds();
    info->mtime = info->atime;
    info->ctime = info->atime;
    info->is_dir = is_dir ? 1u : 0u;
    return 1;
}

static int stat_info_any(const char* path, sys_stat_info_t* info) {
    return virtual_stat_info(path, info) || vfs_stat_info(path, info);
}

static int path_on_pseudo_mount(const char* path) {
    const kernel_mount_t* mnt = mount_find_for_path(path);
    return mnt && mnt->used && mnt->pseudo;
}

static int path_is_mount_target(const char* path) {
    return mount_find_exact(path) != 0;
}

#define SYS_PERM_X 1u
#define SYS_PERM_W 2u
#define SYS_PERM_R 4u
#define SYS_MODE_IFMT  0170000u
#define SYS_MODE_IFDIR 0040000u

static int process_is_root(process_t* proc) {
    return proc && proc->euid == 0u;
}

static int process_in_group(process_t* proc, unsigned int gid) {
    if (!proc) return 0;
    if (proc->egid == gid) return 1;
    return proc->supp_gid_count != 0u && proc->supp_gid == gid;
}

static unsigned int permission_class_bits(process_t* proc, const sys_stat_info_t* info) {
    if (!proc || !info) return 0u;
    if (proc->euid == info->uid) return (info->mode >> 6) & 07u;
    if (process_in_group(proc, info->gid)) return (info->mode >> 3) & 07u;
    return info->mode & 07u;
}

static int permission_allows(process_t* proc,
                             const sys_stat_info_t* info,
                             unsigned int need) {
    if (!proc || !info) return 0;
    if (need == 0u) return 1;
    if (process_is_root(proc)) return 1;
    return (permission_class_bits(proc, info) & need) == need;
}

static int path_parent(char* out, unsigned int out_size, const char* path) {
    int last = -1;

    if (!out || out_size == 0u || !path) return 0;
    for (unsigned int i = 0; path[i] != '\0'; i++) {
        if (path[i] == '/') last = (int)i;
    }
    if (last < 0) {
        if (out_size < 1u) return 0;
        out[0] = '\0';
        return 1;
    }
    if ((unsigned int)last >= out_size) return 0;
    for (int i = 0; i < last; i++) out[i] = path[i];
    out[last] = '\0';
    return 1;
}

static int check_path_prefix_execute(process_t* proc, const char* path) {
    char prefix[PROCESS_FD_NAME_MAX];
    sys_stat_info_t info;

    if (!proc || !path) return -EINVAL;
    if (process_is_root(proc)) return 0;
    for (unsigned int i = 0; path[i] != '\0'; i++) {
        if (path[i] != '/') continue;
        if (i == 0u || i >= sizeof(prefix)) continue;
        k_memcpy(prefix, path, i);
        prefix[i] = '\0';
        if (!stat_info_any(prefix, &info)) return path_lookup_errno(prefix);
        if ((info.mode & SYS_MODE_IFMT) != SYS_MODE_IFDIR) return -ENOTDIR;
        if (!permission_allows(proc, &info, SYS_PERM_X)) return -EACCES;
    }
    return 0;
}

static int check_path_permission(process_t* proc,
                                 const char* path,
                                 unsigned int need,
                                 sys_stat_info_t* out_info) {
    sys_stat_info_t info;
    int rc;

    if (!proc || !path) return -EINVAL;
    rc = check_path_prefix_execute(proc, path);
    if (rc < 0) return rc;
    if (!stat_info_any(path, &info)) return path_lookup_errno(path);
    if (!permission_allows(proc, &info, need)) return -EACCES;
    if (out_info) *out_info = info;
    return 0;
}

static int check_parent_permission(process_t* proc,
                                   const char* path,
                                   unsigned int need) {
    char parent[PROCESS_FD_NAME_MAX];
    sys_stat_info_t info;
    int rc;

    if (!path_parent(parent, sizeof(parent), path)) return -ENAMETOOLONG;
    rc = check_path_prefix_execute(proc, parent);
    if (rc < 0) return rc;
    if (!stat_info_any(parent, &info)) return path_lookup_errno(parent);
    if ((info.mode & SYS_MODE_IFMT) != SYS_MODE_IFDIR) return -ENOTDIR;
    if (!permission_allows(proc, &info, need)) return -EACCES;
    return 0;
}

static unsigned int mode_after_umask(process_t* proc,
                                     unsigned int type,
                                     unsigned int mode) {
    return type | ((mode & 07777u) & ~(proc ? proc->umask : 0022u));
}

/* ------------------------------------------------------------------ */
/* Syscall implementations                                            */
/* ------------------------------------------------------------------ */

static int sys_write_impl(const char* buf, unsigned int len) {
    process_t* proc;
    fd_entry_t* stdout_ent;

    if (len == 0) return 0;
    if (len > SYSCALL_MAX_WRITE_LEN) return -EFBIG;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    proc = (process_t*)sched_current();
    stdout_ent = proc ? process_fd_get(proc, 1) : 0;
    if (!stdout_ent) {
        terminal_write(buf, len);
        return (int)len;
    }
    return process_fd_write(stdout_ent, buf, len);
}

static int sys_putc_impl(unsigned int ch) {
    char c = (char)ch;
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* stdout_ent = proc ? process_fd_get(proc, 1) : 0;

    if (!stdout_ent) {
        terminal_putc(c);
        return 1;
    }
    return process_fd_write(stdout_ent, &c, 1);
}

static void sys_exit_impl(syscall_regs_t* regs) {
    process_t* proc = (process_t*)sched_current();
    if (proc) {
        proc->exit_status = (int)regs->ebx;
    }
    paging_switch(paging_get_kernel_pd());
    sched_exit_current((unsigned int)regs);
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

static unsigned int sys_get_ticks_impl(void) {
    return timer_get_ticks();
}

static void sys_wait_until_current_running(process_t* proc) {
    __asm__ volatile ("sti");
    while (proc && proc->state != PROCESS_STATE_RUNNING) {
        __asm__ volatile ("hlt");
    }
    __asm__ volatile ("cli");
}

static int sys_yield_impl(void) {
    __asm__ volatile ("sti; hlt; cli");
    return 0;
}

/*
 * sys_sleep_impl(regs, ticks)
 *
 * Block the current process until at least ticks timer ticks have elapsed.
 * The task marks itself SLEEPING, stores a wake deadline, then parks with
 * interrupts enabled.  When the timer reaches the deadline the scheduler
 * wakes the task and this function continues.
 */
static int sys_sleep_impl(syscall_regs_t* regs, unsigned int ticks) {
    if (ticks == 0) return 0;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    proc->sleep_until = timer_get_ticks() + ticks;
    proc->state = PROCESS_STATE_SLEEPING;

    /*
     * Park with interrupts enabled.  The next timer IRQ can switch to another
     * runnable task, and this syscall frame resumes once the sleeper is woken.
     */
    (void)regs;
    sys_wait_until_current_running(proc);
    return 0;
}

/*
 * sys_read_impl — true blocking keyboard read.
 *
 * When the keyboard buffer is empty, the calling process is parked:
 *
 *   1. proc->state is set to PROCESS_STATE_WAITING so that
 *      sched_find_next_runnable_from() skips this task on every timer tick.
 *   2. keyboard_set_waiting_process(proc) registers the waiter so that
 *      process_key_consumer() (IRQ1 context) can wake it.
 *   3. sti; hlt — re-enables interrupts and suspends the CPU.
 *
 * While halted the timer IRQ fires normally.  sched_tick() sees that this
 * task is WAITING, skips it, and switches to another runnable task.  When
 * a keypress arrives, process_key_consumer() sets proc->state back to
 * PROCESS_STATE_RUNNING and clears the waiter slot.  On the next timer
 * tick sched_tick() selects this task again; execution resumes after the
 * hlt instruction, re-checks keyboard_buf_available(), finds the character,
 * and continues normally.
 *
 * The outer while loop re-checks the buffer on every wakeup, which
 * correctly handles spurious wakeups (none expected today, but the
 * guard is cheap and correct).
 *
 * IF management:
 *   The syscall gate is an interrupt gate so the CPU clears IF on entry.
 *   We re-enable with sti before the first hlt so IRQ1 can fire.  After
 *   all characters have been collected we restore cli before returning,
 *   matching the expected IF=0 postcondition of the syscall gate.
 */
static int sys_read_impl(char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    return process_fd_read(process_fd_get(proc, 0), buf, len);
}

static int sys_read_raw_impl(char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    return process_fd_read_raw(process_fd_get(proc, 0), buf, len);
}

/*
 * sys_exec_impl(name, argc, argv)
 *
 * Spawn a named ELF program asynchronously.
 *
 * name and argv are user virtual addresses — fully validated before use.
 * name is copied into a kernel-side buffer before calling elf_run_named()
 * because the loader later switches page directories and must not depend
 * on the caller's user pointer remaining valid.
 *
 * argv validation:
 *   1. The argv array base is checked with user_buf_ok() to ensure the
 *      pointer array itself is mapped in user space.
 *   2. Each argv[i] string is copied into a kernel buffer with
 *      copy_user_cstr(), which validates every page it touches and stops
 *      at the first '\0'.  That means elf_run_named() only sees kernel
 *      memory, never caller-owned pointers.
 *   3. The checks happen here, while the caller's CR3 is still active,
 *      so invalid pointers fail before any ELF work begins.
 *
 * Returns 0 on success or a negative errno if validation fails or the
 * program was not found.
 */
static int sys_exec_spawn_impl(const char* name,
                               int argc,
                               char** argv,
                               int new_process_group) {
    char kname[EXEC_NAME_MAX];
    char kargv_data[PROCESS_ARG_BYTES];
    char* kargv[PROCESS_MAX_ARGS + 1];
    unsigned int used = 0;
    process_t* child;
    process_t* parent;

    int name_rc = copy_user_path_resolved(kname, sizeof(kname), name);
    if (name_rc < 0) return name_rc;
    {
        process_t* proc = (process_t*)sched_current();
        int perm_rc = check_path_permission(proc, kname, SYS_PERM_X, 0);
        if (perm_rc < 0) return perm_rc;
    }

    if (argc < 0 || argc > PROCESS_MAX_ARGS) return -EINVAL;

    /* Validate the argv pointer array itself */
    if (argc > 0 && !user_count_bytes_ok((unsigned int)argv,
                                         (unsigned int)argc,
                                         sizeof(char*),
                                         0)) {
        return -EFAULT;
    }

    for (int i = 0; i < argc; i++) {
        int copied = copy_user_cstr(&kargv_data[used],
                                    PROCESS_ARG_BYTES - used,
                                    argv[i]);
        if (copied < 0) return copied == -ENAMETOOLONG ? -EINVAL : copied;
        kargv[i] = &kargv_data[used];
        used += (unsigned int)copied;
    }
    kargv[argc] = 0;

    child = new_process_group ? elf_run_named_new_group(kname, argc, kargv)
                              : elf_run_named(kname, argc, kargv);
    if (!child) return -ENOENT;
    if (new_process_group) {
        parent = (process_t*)sched_current();
        if (process_fd_pty_set_foreground(parent ? process_fd_get(parent, 0) : 0,
                                          child->pgid) < 0) {
            process_set_foreground(child);
        }
    }
    process_claim_for_wait(child);
    return (int)child->pid;
}

static int sys_exec_impl(const char* name, int argc, char** argv) {
    return sys_exec_spawn_impl(name, argc, argv, 0);
}

static int sys_exec_fg_impl(const char* name, int argc, char** argv) {
    return sys_exec_spawn_impl(name, argc, argv, 1);
}

static int sys_copy_user_strv(char** argv,
                              int max_entries,
                              unsigned int data_bytes,
                              int* out_argc,
                              char* kargv_data,
                              char** kargv) {
    unsigned int used = 0;
    int argc = 0;

    if (!out_argc || !kargv_data || !kargv) return -EINVAL;
    if (!argv) {
        *out_argc = 0;
        kargv[0] = 0;
        return 0;
    }

    while (argc < max_entries) {
        char* user_arg = 0;
        int rc;

        if (!user_buf_ok((unsigned int)&argv[argc], sizeof(char*))) {
            return -EFAULT;
        }
        rc = copy_from_user(&user_arg, &argv[argc], sizeof(user_arg));
        if (rc < 0) return rc;
        if (!user_arg) break;

        rc = copy_user_cstr(&kargv_data[used],
                            data_bytes - used,
                            user_arg);
        if (rc < 0) return rc == -ENAMETOOLONG ? -EINVAL : rc;
        kargv[argc] = &kargv_data[used];
        used += (unsigned int)rc;
        argc++;
    }

    if (argc == max_entries) {
        char* extra = 0;
        if (!user_buf_ok((unsigned int)&argv[argc], sizeof(char*))) {
            return -EFAULT;
        }
        if (copy_from_user(&extra, &argv[argc], sizeof(extra)) < 0) {
            return -EFAULT;
        }
        if (extra) return -EINVAL;
    }

    kargv[argc] = 0;
    *out_argc = argc;
    return 0;
}

static int sys_copy_argv(char** argv,
                         int* out_argc,
                         char* kargv_data,
                         char** kargv) {
    return sys_copy_user_strv(argv,
                              PROCESS_MAX_ARGS,
                              PROCESS_ARG_BYTES,
                              out_argc,
                              kargv_data,
                              kargv);
}

static int sys_copy_envp(process_t* proc,
                         char** envp,
                         int* out_envc,
                         char* kenv_data,
                         char** kenvp) {
    unsigned int used = 0;
    int envc;

    if (envp) {
        return sys_copy_user_strv(envp,
                                  PROCESS_MAX_ENVS,
                                  PROCESS_ENV_BYTES,
                                  out_envc,
                                  kenv_data,
                                  kenvp);
    }

    if (!proc || !out_envc || !kenv_data || !kenvp) return -EINVAL;
    envc = proc->user_envc;
    if (envc < 0 || envc > PROCESS_MAX_ENVS) return -EINVAL;

    for (int i = 0; i < envc; i++) {
        int len = proc->user_envp[i] ? k_strlen(proc->user_envp[i]) + 1 : 0;
        if (len <= 0 || used + (unsigned int)len > PROCESS_ENV_BYTES) {
            return -EINVAL;
        }
        kenvp[i] = &kenv_data[used];
        k_memcpy(kenvp[i], proc->user_envp[i], (k_size_t)len);
        used += (unsigned int)len;
    }

    kenvp[envc] = 0;
    *out_envc = envc;
    return 0;
}

static int sys_pipe2_impl(int* user_fds, unsigned int flags) {
    process_t* proc = (process_t*)sched_current();
    int fds[2];
    int rc;

    if (!proc) return -EINVAL;
    if (!user_buf_ok((unsigned int)user_fds, sizeof(fds))) return -EFAULT;
    rc = process_fd_pipe(proc, fds, flags);
    if (rc < 0) return rc;
    if (copy_to_user(user_fds, fds, sizeof(fds)) < 0) {
        sys_close_impl(fds[0]);
        sys_close_impl(fds[1]);
        return -EFAULT;
    }
    return 0;
}

static int sys_pty_open_impl(int* user_fds, unsigned int master_flags) {
    process_t* proc = (process_t*)sched_current();
    int fds[2];
    int rc;

    if (!proc) return -EINVAL;
    if (!user_buf_ok((unsigned int)user_fds, sizeof(fds))) return -EFAULT;
    rc = process_fd_pty(proc, fds, master_flags);
    if (rc < 0) return rc;
    if (copy_to_user(user_fds, fds, sizeof(fds)) < 0) {
        sys_close_impl(fds[0]);
        sys_close_impl(fds[1]);
        return -EFAULT;
    }
    return 0;
}

static int sys_pty_set_size_impl(int fd, unsigned int rows, unsigned int cols) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    return process_fd_pty_set_size(process_fd_get(proc, fd), rows, cols);
}

static int sys_fork_impl(syscall_regs_t* regs) {
    process_t* proc = (process_t*)sched_current();
    process_t* child;
    unsigned int top;

    if (!proc || !proc->kernel_stack_frame) return -EINVAL;
    top = (unsigned int)paging_phys_to_kernel_virt(proc->kernel_stack_frame) +
          proc->kernel_stack_frames * PAGE_SIZE;
    child = process_fork_from_syscall((unsigned int)regs, top);
    if (!child) return -ENOMEM;
    process_claim_for_wait(child);
    return (int)child->pid;
}

static int sys_execve_impl(syscall_regs_t* regs, const char* name, char** argv, char** envp) {
    char kname[EXEC_NAME_MAX];
    char kargv_data[PROCESS_ARG_BYTES];
    char kenv_data[PROCESS_ENV_BYTES];
    char* kargv[PROCESS_MAX_ARGS + 1];
    char* kenvp[PROCESS_MAX_ENVS + 1];
    int argc;
    int envc;
    int rc;
    process_t* proc = (process_t*)sched_current();
    unsigned int entry = 0;
    unsigned int user_esp = 0;
    syscall_iret_frame_t* iret;

    if (!proc) return -EINVAL;
    rc = copy_user_path_resolved(kname, sizeof(kname), name);
    if (rc < 0) return rc;
    rc = check_path_permission(proc, kname, SYS_PERM_X, 0);
    if (rc < 0) return rc;
    rc = sys_copy_argv(argv, &argc, kargv_data, kargv);
    if (rc < 0) return rc;
    rc = sys_copy_envp(proc, envp, &envc, kenv_data, kenvp);
    if (rc < 0) return rc;

    if (!elf_exec_named_into(proc, kname, argc, kargv, envc, kenvp, &entry, &user_esp)) {
        return -ENOENT;
    }

    process_close_cloexec_fds(proc);
    regs->gs = SEG_USER_DATA;
    regs->fs = SEG_USER_DATA;
    regs->es = SEG_USER_DATA;
    regs->ds = SEG_USER_DATA;

    iret = (syscall_iret_frame_t*)((unsigned char*)regs + sizeof(*regs));
    iret->eip = entry;
    iret->cs = SEG_USER_CODE;
    iret->user_esp = user_esp;
    iret->ss = SEG_USER_DATA;
    iret->eflags |= 0x200u;
    return 0;
}

static int wait_status_to_user(int status) {
    if ((status & 0x10000) != 0) {
        return ((status & 0xFF) << 8) | 0x7F;
    }
    if (status >= 128 && status < 256) {
        return status - 128;
    }
    return (status & 0xFF) << 8;
}

static int sys_getpid_impl(void) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    return (int)proc->pid;
}

static int sys_setsid_impl(void) {
    process_t* proc = (process_t*)sched_current();
    return process_setsid(proc);
}

static int sys_getsid_impl(int pid) {
    process_t* proc = (process_t*)sched_current();
    return process_getsid(proc, pid);
}

static int sys_setpgid_impl(int pid, int pgid) {
    process_t* proc = (process_t*)sched_current();
    return process_setpgid(proc, pid, pgid);
}

static int sys_getpgid_impl(int pid) {
    process_t* proc = (process_t*)sched_current();
    return process_getpgid(proc, pid);
}

static int sys_waitpid_impl(int pid, int* user_status, int options) {
    process_t* proc = (process_t*)sched_current();
    int out_pid = 0;
    int raw_status = 0;
    int rc;

    if (!proc) return -EINVAL;
    if (user_status && !user_buf_ok((unsigned int)user_status, sizeof(int))) {
        return -EFAULT;
    }

    rc = process_wait_pid(proc, pid, options, &out_pid, &raw_status);
    if (rc < 0) return rc;

    if (out_pid != 0 && user_status) {
        int encoded = wait_status_to_user(raw_status);
        if (copy_to_user(user_status, &encoded, sizeof(encoded)) < 0) {
            return -EFAULT;
        }
    }

    return out_pid;
}

static int sys_waitpid_fg_impl(int pid, int* user_status) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* stdin_ent;
    process_t* child;
    int raw_status;
    int out_pid = 0;
    int wait_rc;

    if (!proc) return -EINVAL;
    if (pid <= 0) return -EINVAL;
    if (user_status && !user_buf_ok((unsigned int)user_status, sizeof(int))) {
        return -EFAULT;
    }

    child = process_find_by_pid((u32)pid);
    if (!child || child->parent_pid != proc->pid) {
        return -ECHILD;
    }

    child->pgid = child->pid;
    stdin_ent = process_fd_get(proc, 0);
    if (process_fd_pty_set_foreground(stdin_ent, child->pgid) == 0) {
        process_claim_for_wait(child);
        wait_rc = process_wait_pid(proc, pid, 0, &out_pid, &raw_status);
        (void)process_fd_pty_set_foreground(stdin_ent, proc->pgid);
        if (wait_rc < 0) return wait_rc;
    } else {
        raw_status = process_wait_restore_foreground(child, proc);
    }
    if (user_status) {
        int encoded = wait_status_to_user(raw_status);
        if (copy_to_user(user_status, &encoded, sizeof(encoded)) < 0) {
            return -EFAULT;
        }
    }

    return pid;
}

static int sys_kill_impl(syscall_regs_t* regs, int pid, int signum) {
    process_t* proc;
    u32 pgid;

    if (signum < 0 || signum >= 32) return -EINVAL;
    proc = (process_t*)sched_current();
    if (signum == 0) {
        if (pid > 0) return process_find_by_pid((u32)pid) ? 0 : -ESRCH;
        pgid = (pid == 0) ? (proc ? proc->pgid : 0u) : (u32)(-pid);
        return process_find_by_pgid(pgid) ? 0 : -ESRCH;
    }

    if (pid <= 0) {
        pgid = (pid == 0) ? (proc ? proc->pgid : 0u) : (u32)(-pid);
        if (pgid == 0u) return -EINVAL;
        if (signum == 18) return process_group_continue(pgid);
        if (signum == 19 || signum == 20 || signum == 21 || signum == 22) {
            int stopped = process_group_stop(pgid, signum, proc, 0);
            if (stopped && proc && proc->state == PROCESS_STATE_STOPPED) {
                sched_yield_now((unsigned int)regs);
            }
            return stopped ? 0 : -ESRCH;
        }
        return process_group_kill(pgid, 128 + signum) ? 0 : -ESRCH;
    }

    if (signum == 18) return process_continue_pid(pid);
    if (signum == 19 || signum == 20 || signum == 21 || signum == 22) {
        return process_stop_pid(pid, signum, (unsigned int)regs);
    }
    return process_kill_pid(pid, 128 + signum, (unsigned int)regs);
}

/*
 * sys_writefile_impl(name, buf, len)
 *
 * Create or overwrite a root-directory ext2 file in one shot.  This is
 * the historical output primitive for user-space tools.
 */
static int sys_writefile_impl(const char* name, const void* buf, unsigned int len) {
    char kname[EXEC_NAME_MAX];
    int name_rc = copy_user_cstr(kname, sizeof(kname), name);
    if (name_rc < 0) return name_rc;
    if (name_rc <= 1) return -EINVAL;
    if (len > 0 && !user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    return vfs_write_root(kname, (const u8*)buf, len) ? 0 : -EIO;
}

/*
 * sys_writefile_path_impl(path, buf, len)
 *
 * Create or overwrite an ext2 file at an arbitrary path.  This is the
 * preferred output primitive for compilers because it can emit directly
 * into nested directories.
 */
static int sys_writefile_path_impl(const char* path, const void* buf, unsigned int len) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    if (len > 0 && !user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    return vfs_write_path(kpath, (const u8*)buf, len) ? 0 : -EIO;
}

/*
 * heap_page_table_empty(pt)
 *
 * Return 1 if the page table has no present entries.
 */
static int heap_page_table_empty(u32* pt) {
    for (unsigned int i = 0; i < 1024u; i++) {
        if (pt[i] & PAGE_PRESENT) {
            return 0;
        }
    }
    return 1;
}

/*
 * heap_unmap_page(pd, virt)
 *
 * Unmap the page containing virt from the given user page directory.
 * If the page table becomes empty, free it as well.
 */
static void heap_unmap_page(u32* pd, unsigned int virt) {
    unsigned int pd_index = virt >> 22;
    unsigned int pt_index = (virt >> 12) & 0x3FFu;

    u32* pd_virt = (u32*)paging_phys_to_kernel_virt((u32)pd);
    u32 pde = pd_virt[pd_index];
    if (!(pde & PAGE_PRESENT)) {
        return;
    }

    u32 pt_phys = pde & ~0xFFFu;
    u32* pt = (u32*)paging_phys_to_kernel_virt(pt_phys);
    u32 pte = pt[pt_index];
    if (!(pte & PAGE_PRESENT)) {
        return;
    }

    pmm_free_frame(pte & ~0xFFFu);
    pt[pt_index] = 0;
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");

    if (heap_page_table_empty(pt)) {
        pd_virt[pd_index] = 0;
        pmm_free_frame(pt_phys);
    }
}

/*
 * sys_brk_impl(new_brk)
 *
 * Query or adjust the calling process heap break.
 *
 * Passing 0 returns the current break.  Growing the break maps new user
 * pages on demand.  Shrinking the break unmaps whole pages above the new
 * limit and returns the updated value.
 */
static unsigned int sys_brk_impl(unsigned int new_brk) {
    process_t* proc = (process_t*)sched_current();
    if (!proc || !proc->pd) {
        return (unsigned int)-1;
    }

    unsigned int cur_brk = proc->heap_brk;
    if (new_brk == 0) {
        return cur_brk;
    }

    if (new_brk < proc->heap_base || new_brk >= USER_STACK_TOP - USER_STACK_SIZE) {
        return cur_brk;
    }

    if (new_brk == cur_brk) {
        return new_brk;
    }

    u32* pd = proc->pd;

    if (new_brk > cur_brk) {
        unsigned int map_start = PAGE_ALIGN(cur_brk);
        unsigned int map_end = PAGE_ALIGN(new_brk);
        unsigned int mapped = map_start;

        for (unsigned int addr = map_start; addr < map_end; addr += PAGE_SIZE) {
            u32 frame = pmm_alloc_frame();
            if (!frame) {
                for (unsigned int undo = map_start; undo < mapped; undo += PAGE_SIZE) {
                    heap_unmap_page(pd, undo);
                }
                return cur_brk;
            }
            k_memset(paging_phys_to_kernel_virt(frame), 0, PAGE_SIZE);
            paging_map_page(pd, addr, frame, PAGE_WRITE | PAGE_USER);
            mapped = addr + PAGE_SIZE;
        }

        proc->heap_brk = new_brk;
        return new_brk;
    }

    {
        unsigned int unmap_start = PAGE_ALIGN(new_brk);
        unsigned int unmap_end = PAGE_ALIGN(cur_brk);

        for (unsigned int addr = unmap_start; addr < unmap_end; addr += PAGE_SIZE) {
            heap_unmap_page(pd, addr);
        }

        proc->heap_brk = new_brk;
        return new_brk;
    }
}

static int user_page_present_in_pd(u32* pd, unsigned int addr) {
    u32* pd_virt;
    u32 pde;
    u32* pt;
    u32 pte;

    if (!pd) return 0;
    pd_virt = (u32*)paging_phys_to_kernel_virt((u32)pd);
    pde = pd_virt[addr >> 22];
    if (!(pde & PAGE_PRESENT)) return 0;
    pt = (u32*)paging_phys_to_kernel_virt(pde & ~0xFFFu);
    pte = pt[(addr >> 12) & 0x3FFu];
    return (pte & PAGE_PRESENT) != 0u;
}

static int user_mapping_range_ok(unsigned int start, unsigned int length) {
    unsigned int end;

    if (length == 0u) return 0;
    if ((start & (PAGE_SIZE - 1u)) != 0u) return 0;
    if (length > USER_HEAP_BASE - USER_MMAP_BASE) return 0;
    end = start + PAGE_ALIGN(length);
    if (end < start) return 0;
    if (start < USER_MMAP_BASE) return 0;
    if (end > USER_INTERP_BASE) return 0;
    return 1;
}

static int sys_mmap_pick_start(process_t* proc,
                               unsigned int addr,
                               unsigned int size,
                               unsigned int flags,
                               unsigned int* out_start) {
    unsigned int start;

    if (!proc || !out_start) return -EINVAL;
    if (flags & SYS_MAP_FIXED) {
        start = addr & ~(PAGE_SIZE - 1u);
        if (start != addr) return -EINVAL;
    } else {
        start = PAGE_ALIGN(proc->mmap_next ? proc->mmap_next : USER_MMAP_BASE);
        while (start + size <= USER_INTERP_BASE) {
            int occupied = 0;
            for (unsigned int page = start; page < start + size; page += PAGE_SIZE) {
                if (user_page_present_in_pd(proc->pd, page)) {
                    occupied = 1;
                    break;
                }
            }
            if (!occupied) break;
            start += size;
        }
    }

    if (!user_mapping_range_ok(start, size)) return -ENOMEM;

    for (unsigned int page = start; page < start + size; page += PAGE_SIZE) {
        if (user_page_present_in_pd(proc->pd, page)) {
            return -EEXIST;
        }
    }

    *out_start = start;
    return 0;
}

static int sys_mmap_anon_impl(process_t* proc,
                              unsigned int start,
                              unsigned int size,
                              unsigned int prot) {
    unsigned int page_flags = PAGE_USER | ((prot & SYS_PROT_WRITE) ? PAGE_WRITE : 0u);

    for (unsigned int page = start; page < start + size; page += PAGE_SIZE) {
        u32 frame = pmm_alloc_frame();
        if (!frame) {
            for (unsigned int undo = start; undo < page; undo += PAGE_SIZE) {
                heap_unmap_page(proc->pd, undo);
            }
            return -ENOMEM;
        }
        k_memset(paging_phys_to_kernel_virt(frame), 0, PAGE_SIZE);
        paging_map_page(proc->pd, page, frame, page_flags);
    }
    return 0;
}

static int sys_mmap_file_ro_impl(process_t* proc,
                                 unsigned int start,
                                 unsigned int size,
                                 int fd,
                                 unsigned int offset) {
    fd_entry_t* ent;
    u32 file_size = 0;
    int is_dir = 0;

    if ((offset & (PAGE_SIZE - 1u)) != 0u) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (ent->kind != PROCESS_HANDLE_KIND_FILE || !ent->readable ||
        ent->writable || ent->is_dir) {
        return -EBADF;
    }
    if (vfs_file_stat_fd(ent, &file_size, &is_dir) < 0 || is_dir) return -EBADF;
    if (offset >= file_size) return -EINVAL;
    if (size > PAGE_ALIGN(file_size - offset)) return -EINVAL;

    for (unsigned int mapped = 0; mapped < size; mapped += PAGE_SIZE) {
        u32 frame = 0;
        u32 bytes = 0;
        int rc = vfs_file_map_ro_page(ent, offset + mapped, &frame, &bytes);
        (void)bytes;
        if (rc < 0) {
            for (unsigned int undo = 0; undo < mapped; undo += PAGE_SIZE) {
                heap_unmap_page(proc->pd, start + undo);
            }
            return rc;
        }
        paging_map_page(proc->pd,
                        start + mapped,
                        frame,
                        PAGE_USER | PAGE_SHARED_RO_FILE);
    }
    return 0;
}

static int sys_mmap_impl(unsigned int addr,
                         unsigned int length,
                         unsigned int prot,
                         unsigned int flags,
                         int fd,
                         unsigned int offset) {
    process_t* proc = (process_t*)sched_current();
    unsigned int size;
    unsigned int start;
    unsigned int supported_flags = SYS_MAP_PRIVATE | SYS_MAP_FIXED | SYS_MAP_ANON;
    int rc;

    if (!proc || !proc->pd) return -EINVAL;
    if (length == 0u) return -EINVAL;
    size = PAGE_ALIGN(length);
    if (size == 0u || size < length) return -EINVAL;
    if ((flags & ~supported_flags) != 0u) return -EINVAL;
    if ((flags & SYS_MAP_PRIVATE) == 0u) return -EINVAL;

    rc = sys_mmap_pick_start(proc, addr, size, flags, &start);
    if (rc < 0) return rc;

    if (flags & SYS_MAP_ANON) {
        rc = sys_mmap_anon_impl(proc, start, size, prot);
    } else {
        if ((prot & SYS_PROT_WRITE) != 0u) return -ENOSYS;
        if ((prot & SYS_PROT_READ) == 0u) return -EINVAL;
        if ((prot & ~(SYS_PROT_READ | SYS_PROT_EXEC)) != 0u) return -EINVAL;
        rc = sys_mmap_file_ro_impl(proc, start, size, fd, offset);
    }
    if (rc < 0) return rc;

    if (!(flags & SYS_MAP_FIXED) || start + size > proc->mmap_next) {
        proc->mmap_next = start + size;
    }
    return (int)start;
}

static int sys_munmap_impl(unsigned int addr, unsigned int length) {
    process_t* proc = (process_t*)sched_current();
    unsigned int size;

    if (!proc || !proc->pd) return -EINVAL;
    if (length == 0u) return -EINVAL;
    size = PAGE_ALIGN(length);
    if (!user_mapping_range_ok(addr, size)) return -EINVAL;

    for (unsigned int page = addr; page < addr + size; page += PAGE_SIZE) {
        if (user_page_present_in_pd(proc->pd, page)) {
            heap_unmap_page(proc->pd, page);
        }
    }
    return 0;
}

static int sys_mprotect_impl(unsigned int addr, unsigned int length, unsigned int prot) {
    process_t* proc = (process_t*)sched_current();
    unsigned int size;
    u32* pd_virt;

    if (!proc || !proc->pd) return -EINVAL;
    if (length == 0u) return -EINVAL;
    size = PAGE_ALIGN(length);
    if (!user_mapping_range_ok(addr, size)) return -EINVAL;

    pd_virt = (u32*)paging_phys_to_kernel_virt((u32)proc->pd);
    for (unsigned int page = addr; page < addr + size; page += PAGE_SIZE) {
        u32 pde = pd_virt[page >> 22];
        u32* pt;
        u32 idx;
        if (!(pde & PAGE_PRESENT)) return -ENOMEM;
        pt = (u32*)paging_phys_to_kernel_virt(pde & ~0xFFFu);
        idx = (page >> 12) & 0x3FFu;
        if (!(pt[idx] & PAGE_PRESENT)) return -ENOMEM;
        if ((pt[idx] & PAGE_SHARED_RO_FILE) && (prot & SYS_PROT_WRITE)) {
            return -ENOSYS;
        }
        if (prot & SYS_PROT_WRITE) {
            pt[idx] |= PAGE_WRITE;
        } else {
            pt[idx] &= ~PAGE_WRITE;
        }
        __asm__ __volatile__("invlpg (%0)" : : "r"(page) : "memory");
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* File descriptor syscalls                                           */
/* ------------------------------------------------------------------ */

/*
 * sys_open_impl(name)
 *
 * Validate the filename, confirm the file exists through the VFS layer,
 * allocate the lowest free fd slot (>= PROCESS_FD_FIRST)
 * in the current process's fd table, and record name, size, and offset=0.
 *
 * Returns the fd (>= 3) on success or a negative errno on failure.
 */
static int sys_open_impl(const char* name) {
    char kname[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kname, sizeof(kname), name);
    unsigned int vsize = 0;
    unsigned int vtype = PROCESS_VIRTUAL_REGULAR;
    int vis_dir = 0;
    if (path_rc < 0) return path_rc;

    if (virtual_path_exists(kname, &vsize, &vis_dir, &vtype)) {
        process_t* proc;
        char content[PAGE_SIZE];
        const char* data = 0;
        if (vis_dir) return -EISDIR;
        if (vtype == PROCESS_VIRTUAL_REGULAR) {
            vsize = virtual_build_proc_content(kname, content, sizeof(content));
            data = content;
        }
        proc = (process_t*)sched_current();
        if (!proc) return -EINVAL;
        return process_fd_open_virtual(proc, kname, vtype, data, vsize, 1,
                                       vtype == PROCESS_VIRTUAL_NULL ||
                                       vtype == PROCESS_VIRTUAL_TTY);
    }

    u32 file_size = 0;
    int is_dir = 0;
    if (!vfs_stat(kname, &file_size, &is_dir)) return path_lookup_errno(kname);

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    {
        int perm_rc = check_path_permission(proc, kname, SYS_PERM_R, 0);
        if (perm_rc < 0) return perm_rc;
    }

    int fd = process_fd_open_file(proc, kname, file_size, 0);
    fd_entry_t* ent = process_fd_get(proc, fd);
    if (ent) vfs_file_set_is_dir(ent, is_dir);
    return fd;
}

/*
 * sys_close_impl(fd)
 *
 * Mark the fd slot as free.  Returns 0 on success or a negative errno
 * on bad fd/current process state.
 */
static int sys_close_impl(int fd) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;

    process_fd_close(ent);
    return 0;
}

static int sys_open_write_impl(const char* name) {
    char kname[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kname, sizeof(kname), name);
    if (path_rc < 0) return path_rc;
    if (path_on_pseudo_mount(kname)) return -EACCES;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    {
        sys_stat_info_t info;
        int perm_rc;
        if (stat_info_any(kname, &info)) {
            perm_rc = check_path_permission(proc, kname, SYS_PERM_W, 0);
        } else {
            perm_rc = check_parent_permission(proc, kname, SYS_PERM_W | SYS_PERM_X);
        }
        if (perm_rc < 0) return perm_rc;
    }

    int fd = process_fd_open_file(proc, kname, 0, 1);
    if (fd < 0) return fd;
    if (!vfs_write_path(kname, 0, 0)) {
        sys_close_impl(fd);
        return -EIO;
    }
    return fd;
}

static int sys_open_mode_create_kpath_impl(const char* kname,
                                           unsigned int mode,
                                           unsigned int create_mode) {
    unsigned int supported = SYS_OPEN_MODE_READ | SYS_OPEN_MODE_WRITE |
                             SYS_OPEN_MODE_CREATE | SYS_OPEN_MODE_TRUNC |
                             SYS_OPEN_MODE_APPEND | SYS_OPEN_MODE_EXCL;
    int readable = (mode & SYS_OPEN_MODE_READ) != 0;
    int writable = (mode & SYS_OPEN_MODE_WRITE) != 0;
    int create = (mode & SYS_OPEN_MODE_CREATE) != 0;
    int trunc = (mode & SYS_OPEN_MODE_TRUNC) != 0;
    int append = (mode & SYS_OPEN_MODE_APPEND) != 0;
    int excl = (mode & SYS_OPEN_MODE_EXCL) != 0;
    u32 file_size = 0;
    int is_dir = 0;
    int exists;
    int fd;
    process_t* proc;

    if ((mode & ~supported) != 0) return -EINVAL;
    if (!readable && !writable) return -EINVAL;
    if ((trunc || append || create) && !writable) return -EINVAL;

    {
        unsigned int vsize = 0;
        unsigned int vtype = PROCESS_VIRTUAL_REGULAR;
        int vis_dir = 0;
        if (virtual_path_exists(kname, &vsize, &vis_dir, &vtype)) {
            char content[PAGE_SIZE];
            const char* data = 0;
            if (create || trunc || excl || append) return -EISDIR;
            if (vis_dir) return -EISDIR;
            if (writable && vtype != PROCESS_VIRTUAL_NULL && vtype != PROCESS_VIRTUAL_TTY) {
                return -EBADF;
            }
            if (vtype == PROCESS_VIRTUAL_REGULAR) {
                vsize = virtual_build_proc_content(kname, content, sizeof(content));
                data = content;
            }
            process_t* proc = (process_t*)sched_current();
            if (!proc) return -EINVAL;
            return process_fd_open_virtual(proc, kname, vtype, data, vsize,
                                           readable,
                                           writable ||
                                           vtype == PROCESS_VIRTUAL_NULL ||
                                           vtype == PROCESS_VIRTUAL_TTY);
        }
    }

    exists = vfs_stat(kname, &file_size, &is_dir);
    if (!exists && path_on_pseudo_mount(kname)) return create ? -EACCES : -ENOENT;
    if (exists && create && excl) return -EEXIST;
    if (exists && is_dir && writable) return -EISDIR;
    if (!exists && !create) return path_lookup_errno(kname);
    if (!exists || trunc) file_size = 0;

    proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    if (exists) {
        unsigned int need = 0u;
        if (readable) need |= SYS_PERM_R;
        if (writable) need |= SYS_PERM_W;
        {
            int perm_rc = check_path_permission(proc, kname, need, 0);
            if (perm_rc < 0) return perm_rc;
        }
    } else {
        int perm_rc = check_parent_permission(proc, kname, SYS_PERM_W | SYS_PERM_X);
        if (perm_rc < 0) return perm_rc;
    }

    fd = process_fd_open_file_mode(proc, kname, file_size, readable, writable);
    if (fd < 0) return fd;

    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) {
        sys_close_impl(fd);
        return -EBADF;
    }
    vfs_file_set_is_dir(ent, (exists && is_dir) ? 1 : 0);

    if (!exists || trunc) {
        if (!vfs_write_path(kname, 0, 0)) {
            sys_close_impl(fd);
            return -EIO;
        }
        if (!exists) {
            unsigned int final_mode = mode_after_umask(proc, 0100000u, create_mode);
            (void)vfs_chmod(kname, (u16)final_mode);
            (void)vfs_chown(kname, (u16)proc->euid, (u16)proc->egid);
        }
    }

    if (append) {
        if (process_fd_seek(ent, 0, 2) < 0) {
            sys_close_impl(fd);
            return -EINVAL;
        }
    }

    return fd;
}

static int sys_open_mode_create_impl(const char* name,
                                     unsigned int mode,
                                     unsigned int create_mode) {
    char kname[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kname, sizeof(kname), name);
    if (path_rc < 0) return path_rc;
    return sys_open_mode_create_kpath_impl(kname, mode, create_mode);
}

static int sys_openat_mode_create_impl(int dirfd,
                                       const char* name,
                                       unsigned int mode,
                                       unsigned int create_mode) {
    char kname[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_at_resolved(kname, sizeof(kname), dirfd, name);
    if (path_rc < 0) return path_rc;
    return sys_open_mode_create_kpath_impl(kname, mode, create_mode);
}

static int sys_open_mode_impl(const char* name, unsigned int mode) {
    return sys_open_mode_create_impl(name, mode, 0666u);
}

static int copy_user_sockaddr_in(struct sockaddr_in* dst,
                                 const struct sockaddr* src,
                                 unsigned int len) {
    if (!dst || !src) {
        return -EFAULT;
    }
    if (len < sizeof(struct sockaddr_in)) {
        return -EINVAL;
    }
    if (!user_buf_ok((unsigned int)src, sizeof(struct sockaddr_in))) {
        return -EFAULT;
    }

    if (copy_from_user(dst, src, sizeof(struct sockaddr_in)) < 0) {
        return -EFAULT;
    }
    if (dst->sin_family != AF_INET) {
        return -EINVAL;
    }
    return 0;
}

static int socket_fd_is_socket(fd_entry_t* ent) {
    return ent && ent->valid && ent->kind == PROCESS_HANDLE_KIND_SOCKET && ent->socket;
}

static unsigned short swap_u16(unsigned short value) {
    return (unsigned short)(((value & 0x00FFu) << 8) | ((value & 0xFF00u) >> 8));
}

static unsigned int swap_u32(unsigned int value) {
    return ((value & 0x000000FFu) << 24)
         | ((value & 0x0000FF00u) << 8)
         | ((value & 0x00FF0000u) >> 8)
         | ((value & 0xFF000000u) >> 24);
}

static int sys_socket_impl(int domain, int type, int protocol) {
    process_t* proc = (process_t*)sched_current();
    int fd;
    int sock_type = type & SOCK_TYPE_MASK;
    unsigned int fd_flags = 0u;
    socket_kind_t kind = SOCKET_KIND_NONE;

    if (!proc) return -EINVAL;
    if ((type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC)) != 0) return -EINVAL;
    if (domain == AF_NETLINK) {
        if (protocol != NETLINK_ROUTE) return -EPROTONOSUPPORT;
        if (sock_type != SOCK_RAW && sock_type != SOCK_DGRAM) return -EPROTONOSUPPORT;
        kind = SOCKET_KIND_NETLINK_ROUTE;
    } else if (domain != AF_INET) {
        return -EAFNOSUPPORT;
    } else if (sock_type == SOCK_STREAM) {
        if (protocol != 0 && protocol != IPPROTO_TCP) return -EPROTONOSUPPORT;
        kind = SOCKET_KIND_TCP;
    } else if (sock_type == SOCK_DGRAM) {
        if (protocol != 0 && protocol != IPPROTO_UDP) return -EPROTONOSUPPORT;
        kind = SOCKET_KIND_UDP;
    } else if (sock_type == SOCK_RAW) {
        if (protocol != IPPROTO_ICMP) return -EPROTONOSUPPORT;
        kind = SOCKET_KIND_RAW_ICMP;
    } else {
        return -EPROTONOSUPPORT;
    }

    fd = process_fd_open_socket_kind(proc, "socket", kind);
    if (fd < 0) return fd;
    if ((type & SOCK_NONBLOCK) != 0) {
        fd_entry_t* ent = process_fd_get(proc, fd);
        fd_flags |= SYS_FD_FLAG_NONBLOCK;
        (void)process_fd_set_flags(ent, fd_flags);
    }
    if ((type & SOCK_CLOEXEC) != 0) {
        fd_entry_t* ent = process_fd_get(proc, fd);
        (void)process_fd_set_fd_flags(ent, SYS_FD_FLAG_CLOEXEC);
    }
    return fd;
}

static int sys_bind_impl(int fd, const struct sockaddr* addr, unsigned int addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_state(ent->socket) != SOCKET_STATE_OPEN) return -EINVAL;
    if (socket_kind(ent->socket) == SOCKET_KIND_NETLINK_ROUTE) {
        struct sockaddr_nl nl;
        if (!addr || addrlen < sizeof(nl)) return -EINVAL;
        if (!user_buf_ok((unsigned int)addr, sizeof(nl))) return -EFAULT;
        if (copy_from_user(&nl, addr, sizeof(nl)) < 0) return -EFAULT;
        if (nl.nl_family != AF_NETLINK) return -EINVAL;
        if (nl.nl_pid == 0u) nl.nl_pid = (unsigned int)proc->pid;
        return socket_bind_netlink(ent->socket, nl.nl_pid, nl.nl_groups);
    }
    int sa_rc = copy_user_sockaddr_in(&sa, addr, addrlen);
    if (sa_rc < 0) return sa_rc;

    if (socket_kind(ent->socket) == SOCKET_KIND_UDP) {
        sa_rc = socket_bind_udp(ent->socket, swap_u16(sa.sin_port));
    } else {
        sa_rc = socket_bind_tcp(ent->socket, swap_u16(sa.sin_port));
    }
    if (sa_rc < 0) return sa_rc;
    ent->socket_port = socket_local_port(ent->socket);
    ent->socket_state = PROCESS_SOCKET_STATE_BOUND;
    return 0;
}

static int sys_listen_impl(int fd, int backlog) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    (void)backlog;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_state(ent->socket) != SOCKET_STATE_BOUND) return -EINVAL;
    if (socket_local_port(ent->socket) == 0u) return -EINVAL;

    int listen_rc = socket_listen_tcp(ent->socket, backlog);
    if (listen_rc < 0) return listen_rc;
    ent->socket_port = socket_local_port(ent->socket);
    ent->socket_state = PROCESS_SOCKET_STATE_LISTENER;
    return 0;
}

static int sys_accept_impl(syscall_regs_t* regs,
                           int fd,
                           struct sockaddr* addr,
                           unsigned int* addrlen,
                           unsigned int flags) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    fd_entry_t* new_ent;
    unsigned int peer_ip;
    unsigned int peer_port;
    int new_fd;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_state(ent->socket) != SOCKET_STATE_LISTENING) return -EINVAL;
    if ((flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) != 0u) return -EINVAL;

    if (!socket_accept_ready(ent->socket) &&
        ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u ||
         (flags & SOCK_NONBLOCK) != 0u)) {
        return -EAGAIN;
    }

    while (!socket_accept_ready(ent->socket)) {
        int wait_rc;

        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_accept_ready(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);

    new_fd = process_fd_open_socket(proc, "socket");
    if (new_fd < 0) return new_fd;
    new_ent = process_fd_get(proc, new_fd);
    if (!socket_fd_is_socket(new_ent)) {
        process_fd_close(new_ent);
        return -EBADF;
    }
    if (socket_accept_tcp(ent->socket, new_ent->socket) < 0) {
        process_fd_close(new_ent);
        return -EAGAIN;
    }
    new_ent->socket_state = PROCESS_SOCKET_STATE_CONNECTED;
    new_ent->socket_port = socket_local_port(new_ent->socket);
    new_ent->socket_conn = socket_conn_id(new_ent->socket);
    if ((flags & SOCK_NONBLOCK) != 0u) {
        new_ent->flags |= SYS_FD_FLAG_NONBLOCK;
    }
    if ((flags & SOCK_CLOEXEC) != 0u) {
        new_ent->fd_flags |= SYS_FD_FLAG_CLOEXEC;
    }

    peer_ip = socket_peer_ip(new_ent->socket);
    peer_port = socket_peer_port(new_ent->socket);
    if (addr && addrlen) {
        struct sockaddr_in sa;
        unsigned int user_addrlen = 0;
        if (read_user_u32(&user_addrlen, addrlen) < 0) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -EFAULT;
        }
        if (user_addrlen < sizeof(struct sockaddr_in)) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -EINVAL;
        }
        if (!user_buf_ok((unsigned int)addr, sizeof(struct sockaddr_in))) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -EFAULT;
        }
        k_memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = swap_u16((unsigned short)peer_port);
        sa.sin_addr.s_addr = swap_u32(peer_ip);
        if (copy_to_user(addr, &sa, sizeof(sa)) < 0 ||
            write_user_u32(addrlen, sizeof(sa)) < 0) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -EFAULT;
        }
    }

    return new_fd;
}

static int sys_connect_impl(int fd, const struct sockaddr* addr, unsigned int addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;
    unsigned int remote_ip;
    unsigned int remote_port;
    int rc;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    rc = copy_user_sockaddr_in(&sa, addr, addrlen);
    if (rc < 0) return rc;

    remote_ip = swap_u32(sa.sin_addr.s_addr);
    remote_port = swap_u16(sa.sin_port);
    if (socket_kind(ent->socket) == SOCKET_KIND_UDP) {
        rc = socket_connect_udp(ent->socket, remote_ip, remote_port);
        if (rc < 0) return rc;
        ent->socket_state = PROCESS_SOCKET_STATE_CONNECTED;
        ent->socket_port = socket_local_port(ent->socket);
        return 0;
    }

    rc = socket_connect_tcp(ent->socket, remote_ip, remote_port);
    if (rc == -EALREADY && (ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
        return -EALREADY;
    }
    if (rc < 0) return rc;

    ent->socket_state = socket_tcp_connection_established(ent->socket)
                      ? PROCESS_SOCKET_STATE_CONNECTED
                      : PROCESS_SOCKET_STATE_CONNECTING;
    ent->socket_port = socket_local_port(ent->socket);
    ent->socket_conn = socket_conn_id(ent->socket);

    if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
        if (!socket_tcp_connection_established(ent->socket)) {
            return -EINPROGRESS;
        }
        ent->socket_state = PROCESS_SOCKET_STATE_CONNECTED;
        return 0;
    }

    while (!socket_tcp_connection_established(ent->socket)) {
        int wait_rc;

        if (!socket_tcp_connect_pending(ent->socket)) {
            socket_wait_clear_process(proc);
            return -ECONNREFUSED;
        }
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLOUT);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_tcp_connection_established(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);
    ent->socket_state = PROCESS_SOCKET_STATE_CONNECTED;
    return 0;
}

typedef struct {
    unsigned char* buf;
    unsigned int cap;
    unsigned int len;
    unsigned int seq;
    unsigned int pid;
    unsigned int multi;
} netlink_builder_t;

static unsigned int net_mask_from_prefix(unsigned int prefix) {
    if (prefix == 0u) return 0u;
    if (prefix >= 32u) return 0xFFFFFFFFu;
    return 0xFFFFFFFFu << (32u - prefix);
}

static unsigned int net_prefix_from_mask(unsigned int mask) {
    unsigned int prefix = 0u;
    for (int bit = 31; bit >= 0; bit--) {
        if ((mask & (1u << (unsigned int)bit)) == 0u) break;
        prefix++;
    }
    return prefix;
}

static unsigned int net_eth0_flags(void) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    unsigned int flags = IFF_BROADCAST | IFF_MULTICAST;
    if (cfg && cfg->configured) flags |= IFF_UP;
    if (nic_link_up()) flags |= IFF_RUNNING;
    return flags;
}

static void* nl_begin(netlink_builder_t* b,
                      unsigned int type,
                      unsigned int payload_len,
                      unsigned int flags) {
    unsigned int off;
    unsigned int total;
    struct nlmsghdr* nlh;

    if (!b || !b->buf) return 0;
    off = NLMSG_ALIGN(b->len);
    total = NLMSG_LENGTH(payload_len);
    if (off + total > b->cap) return 0;
    if (off > b->len) {
        k_memset(b->buf + b->len, 0, off - b->len);
    }
    nlh = (struct nlmsghdr*)(b->buf + off);
    k_memset(nlh, 0, total);
    nlh->nlmsg_len = total;
    nlh->nlmsg_type = (unsigned short)type;
    nlh->nlmsg_flags = (unsigned short)(flags | (b->multi ? NLM_F_MULTI : 0u));
    nlh->nlmsg_seq = b->seq;
    nlh->nlmsg_pid = b->pid;
    b->len = off + total;
    return NLMSG_DATA(nlh);
}

static int nl_attr_put(netlink_builder_t* b,
                       struct nlmsghdr* nlh,
                       unsigned int type,
                       const void* data,
                       unsigned int len) {
    unsigned int msg_off;
    unsigned int attr_off;
    unsigned int attr_len = RTA_LENGTH(len);
    struct rtattr* rta;

    if (!b || !nlh || !data) return -EINVAL;
    msg_off = (unsigned int)((unsigned char*)nlh - b->buf);
    attr_off = msg_off + NLMSG_ALIGN(nlh->nlmsg_len);
    if (attr_off + attr_len > b->cap) return -EMSGSIZE;
    if (attr_off > b->len) {
        k_memset(b->buf + b->len, 0, attr_off - b->len);
    }
    rta = (struct rtattr*)(b->buf + attr_off);
    k_memset(rta, 0, RTA_ALIGN(attr_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)attr_len;
    k_memcpy(RTA_DATA(rta), data, len);
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + attr_len;
    b->len = msg_off + nlh->nlmsg_len;
    return 0;
}

static int nl_attr_put_u32(netlink_builder_t* b,
                           struct nlmsghdr* nlh,
                           unsigned int type,
                           unsigned int value) {
    return nl_attr_put(b, nlh, type, &value, sizeof(value));
}

static int nl_attr_put_str(netlink_builder_t* b,
                           struct nlmsghdr* nlh,
                           unsigned int type,
                           const char* value) {
    return nl_attr_put(b, nlh, type, value, (unsigned int)k_strlen(value) + 1u);
}

static int nl_put_done(netlink_builder_t* b) {
    return nl_begin(b, NLMSG_DONE, 0u, 0u) ? 0 : -EMSGSIZE;
}

static int nl_put_ack(netlink_builder_t* b,
                      const struct nlmsghdr* req,
                      int error) {
    struct nlmsgerr* err = (struct nlmsgerr*)nl_begin(b, NLMSG_ERROR,
                                                      sizeof(*err), 0u);
    if (!err) return -EMSGSIZE;
    err->error = error;
    if (req) {
        err->msg = *req;
    } else {
        k_memset(&err->msg, 0, sizeof(err->msg));
    }
    return 0;
}

static int nl_put_link(netlink_builder_t* b) {
    struct ifinfomsg* ifi;
    struct nlmsghdr* nlh;
    const u8* mac = nic_mac();
    unsigned char bcast[ETH_ALEN];
    unsigned int mtu = 1500u;
    unsigned char operstate = 6u;

    ifi = (struct ifinfomsg*)nl_begin(b, RTM_NEWLINK, sizeof(*ifi), 0u);
    if (!ifi) return -EMSGSIZE;
    nlh = (struct nlmsghdr*)((unsigned char*)ifi - NLMSG_LENGTH(0));
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_type = ARPHRD_ETHER;
    ifi->ifi_index = 1;
    ifi->ifi_flags = net_eth0_flags();
    ifi->ifi_change = 0xFFFFFFFFu;

    k_memset(bcast, 0xFF, sizeof(bcast));
    if (nl_attr_put_str(b, nlh, IFLA_IFNAME, "eth0") < 0) return -EMSGSIZE;
    if (mac && nl_attr_put(b, nlh, IFLA_ADDRESS, mac, ETH_ALEN) < 0) return -EMSGSIZE;
    if (nl_attr_put(b, nlh, IFLA_BROADCAST, bcast, ETH_ALEN) < 0) return -EMSGSIZE;
    if (nl_attr_put_u32(b, nlh, IFLA_MTU, mtu) < 0) return -EMSGSIZE;
    if (nl_attr_put(b, nlh, IFLA_OPERSTATE, &operstate, sizeof(operstate)) < 0) {
        return -EMSGSIZE;
    }
    return 0;
}

static int nl_put_addr(netlink_builder_t* b) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    struct ifaddrmsg* ifa;
    struct nlmsghdr* nlh;
    unsigned int ip_be;
    unsigned int bcast_be;

    if (!cfg || !cfg->configured || cfg->ip == 0u) return 0;
    ifa = (struct ifaddrmsg*)nl_begin(b, RTM_NEWADDR, sizeof(*ifa), 0u);
    if (!ifa) return -EMSGSIZE;
    nlh = (struct nlmsghdr*)((unsigned char*)ifa - NLMSG_LENGTH(0));
    ifa->ifa_family = AF_INET;
    ifa->ifa_prefixlen = (unsigned char)net_prefix_from_mask(cfg->netmask);
    ifa->ifa_flags = IFA_F_PERMANENT;
    ifa->ifa_scope = RT_SCOPE_UNIVERSE;
    ifa->ifa_index = 1u;

    ip_be = swap_u32(cfg->ip);
    bcast_be = swap_u32((cfg->ip & cfg->netmask) | (~cfg->netmask));
    if (nl_attr_put_u32(b, nlh, IFA_ADDRESS, ip_be) < 0) return -EMSGSIZE;
    if (nl_attr_put_u32(b, nlh, IFA_LOCAL, ip_be) < 0) return -EMSGSIZE;
    if (nl_attr_put_u32(b, nlh, IFA_BROADCAST, bcast_be) < 0) return -EMSGSIZE;
    if (nl_attr_put_str(b, nlh, IFA_LABEL, "eth0") < 0) return -EMSGSIZE;
    return 0;
}

static int nl_put_route(netlink_builder_t* b,
                        unsigned int dst,
                        unsigned int prefix,
                        unsigned int gateway,
                        unsigned int scope,
                        unsigned int proto) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    struct rtmsg* rtm;
    struct nlmsghdr* nlh;
    unsigned int ifindex = 1u;

    if (!cfg || !cfg->configured) return 0;
    rtm = (struct rtmsg*)nl_begin(b, RTM_NEWROUTE, sizeof(*rtm), 0u);
    if (!rtm) return -EMSGSIZE;
    nlh = (struct nlmsghdr*)((unsigned char*)rtm - NLMSG_LENGTH(0));
    rtm->rtm_family = AF_INET;
    rtm->rtm_dst_len = (unsigned char)prefix;
    rtm->rtm_table = RT_TABLE_MAIN;
    rtm->rtm_protocol = (unsigned char)proto;
    rtm->rtm_scope = (unsigned char)scope;
    rtm->rtm_type = RTN_UNICAST;

    if (prefix != 0u) {
        unsigned int dst_be = swap_u32(dst);
        if (nl_attr_put_u32(b, nlh, RTA_DST, dst_be) < 0) return -EMSGSIZE;
    }
    if (gateway != 0u) {
        unsigned int gw_be = swap_u32(gateway);
        if (nl_attr_put_u32(b, nlh, RTA_GATEWAY, gw_be) < 0) return -EMSGSIZE;
    }
    if (nl_attr_put_u32(b, nlh, RTA_OIF, ifindex) < 0) return -EMSGSIZE;
    if (cfg->ip != 0u) {
        unsigned int src_be = swap_u32(cfg->ip);
        if (nl_attr_put_u32(b, nlh, RTA_PREFSRC, src_be) < 0) return -EMSGSIZE;
    }
    return 0;
}

static void nl_parse_attrs(struct rtattr** attrs,
                           unsigned int max,
                           struct rtattr* rta,
                           int len) {
    for (unsigned int i = 0; i <= max; i++) attrs[i] = 0;
    while (RTA_OK(rta, len)) {
        if (rta->rta_type <= max) attrs[rta->rta_type] = rta;
        rta = RTA_NEXT(rta, len);
    }
}

static unsigned int nl_attr_u32(struct rtattr* rta) {
    unsigned int out = 0u;
    if (rta && RTA_PAYLOAD(rta) >= (int)sizeof(out)) {
        k_memcpy(&out, RTA_DATA(rta), sizeof(out));
    }
    return out;
}

static int nl_apply_addr(const struct nlmsghdr* nlh, int del) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    const struct ifaddrmsg* ifa = (const struct ifaddrmsg*)NLMSG_DATA(nlh);
    struct rtattr* attrs[IFA_MAX + 1u];
    unsigned int ip = cfg ? cfg->ip : 0u;
    unsigned int netmask = cfg && cfg->netmask ? cfg->netmask : 0xFFFFFF00u;
    unsigned int gateway = cfg ? cfg->gateway : 0u;
    unsigned int dns = cfg ? cfg->dns : 0u;
    unsigned int dhcp_server = cfg ? cfg->dhcp_server : 0u;
    unsigned int lease_seconds = cfg ? cfg->lease_seconds : 0u;
    unsigned int ip_be;

    if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*ifa))) return -EINVAL;
    if (ifa->ifa_family != AF_INET || ifa->ifa_index != 1u) return -ENODEV;
    nl_parse_attrs(attrs, IFA_MAX, IFA_RTA(ifa), IFA_PAYLOAD(nlh));
    ip_be = nl_attr_u32(attrs[IFA_LOCAL] ? attrs[IFA_LOCAL] : attrs[IFA_ADDRESS]);
    if (del) {
        if (ip_be != 0u && cfg && swap_u32(ip_be) != cfg->ip) return -EADDRNOTAVAIL;
        net_ipv4_configure(0u, netmask, gateway, dns, dhcp_server, lease_seconds);
        return 0;
    }
    ip = swap_u32(ip_be);
    if (ip == 0u) return -EINVAL;
    netmask = net_mask_from_prefix(ifa->ifa_prefixlen);
    net_ipv4_configure(ip, netmask, gateway, dns, dhcp_server, lease_seconds);
    return 0;
}

static int nl_apply_route(const struct nlmsghdr* nlh, int del) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    const struct rtmsg* rtm = (const struct rtmsg*)NLMSG_DATA(nlh);
    struct rtattr* attrs[RTA_MAX + 1u];
    unsigned int dst = 0u;
    unsigned int gateway = 0u;

    if (!cfg || !cfg->configured) return -ENETUNREACH;
    if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*rtm))) return -EINVAL;
    if (rtm->rtm_family != AF_INET) return -EAFNOSUPPORT;
    nl_parse_attrs(attrs, RTA_MAX, RTM_RTA(rtm), RTM_PAYLOAD(nlh));
    if (attrs[RTA_DST]) dst = swap_u32(nl_attr_u32(attrs[RTA_DST]));
    if (attrs[RTA_GATEWAY]) gateway = swap_u32(nl_attr_u32(attrs[RTA_GATEWAY]));
    if (attrs[RTA_OIF] && nl_attr_u32(attrs[RTA_OIF]) != 1u) return -ENODEV;

    if (rtm->rtm_dst_len != 0u || dst != 0u) {
        return del ? 0 : -EOPNOTSUPP;
    }
    net_ipv4_configure(cfg->ip,
                       cfg->netmask,
                       del ? 0u : gateway,
                       cfg->dns,
                       cfg->dhcp_server,
                       cfg->lease_seconds);
    return 0;
}

static int nl_build_response(socket_t* sock,
                             const unsigned char* req_buf,
                             unsigned int req_len,
                             unsigned char* resp,
                             unsigned int resp_cap) {
    netlink_builder_t b;
    struct nlmsghdr* nlh;
    int remaining;
    int rc = 0;

    if (!sock || !req_buf || !resp || req_len < sizeof(struct nlmsghdr)) return -EINVAL;
    nlh = (struct nlmsghdr*)req_buf;
    remaining = (int)req_len;
    if (!NLMSG_OK(nlh, remaining)) return -EINVAL;

    k_memset(&b, 0, sizeof(b));
    b.buf = resp;
    b.cap = resp_cap;
    b.seq = nlh->nlmsg_seq;
    b.pid = socket_netlink_pid(sock);

    switch (nlh->nlmsg_type) {
    case RTM_GETLINK:
        b.multi = 1u;
        rc = nl_put_link(&b);
        if (rc == 0) rc = nl_put_done(&b);
        break;
    case RTM_GETADDR:
        b.multi = 1u;
        rc = nl_put_addr(&b);
        if (rc == 0) rc = nl_put_done(&b);
        break;
    case RTM_GETROUTE:
    {
        const net_ipv4_config_t* cfg = net_ipv4_config();
        b.multi = 1u;
        if (cfg && cfg->configured) {
            unsigned int prefix = net_prefix_from_mask(cfg->netmask);
            unsigned int network = cfg->ip & cfg->netmask;
            if (prefix != 0u) {
                rc = nl_put_route(&b, network, prefix, 0u,
                                  RT_SCOPE_LINK, RTPROT_KERNEL);
            }
            if (rc == 0 && cfg->gateway != 0u) {
                rc = nl_put_route(&b, 0u, 0u, cfg->gateway,
                                  RT_SCOPE_UNIVERSE, RTPROT_DHCP);
            }
        }
        if (rc == 0) rc = nl_put_done(&b);
        break;
    }
    case RTM_NEWADDR:
        rc = nl_apply_addr(nlh, 0);
        (void)nl_put_ack(&b, nlh, rc < 0 ? rc : 0);
        break;
    case RTM_DELADDR:
        rc = nl_apply_addr(nlh, 1);
        (void)nl_put_ack(&b, nlh, rc < 0 ? rc : 0);
        break;
    case RTM_NEWROUTE:
        rc = nl_apply_route(nlh, 0);
        (void)nl_put_ack(&b, nlh, rc < 0 ? rc : 0);
        break;
    case RTM_DELROUTE:
        rc = nl_apply_route(nlh, 1);
        (void)nl_put_ack(&b, nlh, rc < 0 ? rc : 0);
        break;
    case RTM_SETLINK:
    case RTM_NEWLINK:
        (void)nl_put_ack(&b, nlh, 0);
        break;
    default:
        (void)nl_put_ack(&b, nlh, -EOPNOTSUPP);
        break;
    }

    if (b.len == 0u) return rc < 0 ? rc : -EINVAL;
    return socket_netlink_queue(sock, b.buf, b.len) < 0 ? -ENOMEM : (int)req_len;
}

static int sys_netlink_send_user(fd_entry_t* ent,
                                 const void* buf,
                                 unsigned int len) {
    static unsigned char req[SYS_NETLINK_MAX_PACKET];
    static unsigned char resp[SYS_NETLINK_MAX_PACKET];

    if (!ent || !ent->socket || socket_kind(ent->socket) != SOCKET_KIND_NETLINK_ROUTE) {
        return -EINVAL;
    }
    if (!buf || len == 0u) return -EINVAL;
    if (len > sizeof(req)) return -EMSGSIZE;
    if (copy_from_user(req, buf, len) < 0) return -EFAULT;
    k_memset(resp, 0, sizeof(resp));
    return nl_build_response(ent->socket, req, len, resp, sizeof(resp));
}

static int sys_netlink_recv_user(process_t* proc,
                                 fd_entry_t* ent,
                                 void* buf,
                                 unsigned int len,
                                 unsigned int flags,
                                 struct sockaddr* src_addr,
                                 unsigned int* addrlen) {
    static unsigned char packet[SYS_NETLINK_MAX_PACKET];
    unsigned int src_pid = 0u;
    unsigned int src_groups = 0u;
    unsigned int peek = (flags & MSG_PEEK) != 0u;
    int rc;

    if (!proc || !ent || !buf) return -EINVAL;
    if (socket_kind(ent->socket) != SOCKET_KIND_NETLINK_ROUTE) return -EINVAL;
    if (!socket_netlink_recv_ready(ent->socket) &&
        ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u ||
         (flags & MSG_DONTWAIT) != 0u)) {
        return -EAGAIN;
    }
    while (!socket_netlink_recv_ready(ent->socket)) {
        int wait_rc;
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_netlink_recv_ready(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);

    rc = socket_netlink_recv(ent->socket, packet, sizeof(packet),
                             &src_pid, &src_groups, peek);
    if (rc < 0) return rc;
    if ((unsigned int)rc > len) {
        rc = (int)len;
    }
    if (copy_to_user(buf, packet, (unsigned int)rc) < 0) return -EFAULT;
    if (src_addr && addrlen) {
        struct sockaddr_nl sa;
        unsigned int user_addrlen = 0u;
        if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
        if (user_addrlen < sizeof(sa)) return -EINVAL;
        if (!user_buf_ok((unsigned int)src_addr, sizeof(sa))) return -EFAULT;
        k_memset(&sa, 0, sizeof(sa));
        sa.nl_family = AF_NETLINK;
        sa.nl_pid = src_pid;
        sa.nl_groups = src_groups;
        if (copy_to_user(src_addr, &sa, sizeof(sa)) < 0 ||
            write_user_u32(addrlen, sizeof(sa)) < 0) {
            return -EFAULT;
        }
    } else if (src_addr || addrlen) {
        return -EFAULT;
    }
    return rc;
}

static int sys_udp_send_socket(fd_entry_t* ent,
                               const void* buf,
                               unsigned int len,
                               const struct sockaddr* dest_addr,
                               unsigned int addrlen);
static void net_write_u16_be(unsigned char* buf, unsigned int off, unsigned int value);

static int sys_send_impl(int fd, const void* buf, unsigned int len) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    if (len == 0u) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_kind(ent->socket) == SOCKET_KIND_NETLINK_ROUTE) {
        return sys_netlink_send_user(ent, buf, len);
    }
    if (socket_kind(ent->socket) == SOCKET_KIND_UDP) {
        return sys_udp_send_socket(ent, buf, len, 0, 0u);
    }
    return process_fd_write(ent, (const char*)buf, len);
}

static int sys_raw_icmp_recv_socket(process_t* proc,
                                    fd_entry_t* ent,
                                    void* buf,
                                    unsigned int len,
                                    unsigned int flags,
                                    unsigned int* out_src_ip) {
    if (!proc || !ent || !buf) return -EINVAL;
    if (!socket_raw_icmp_recv_ready(ent->socket) &&
        ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u ||
         (flags & MSG_DONTWAIT) != 0u)) {
        return -EAGAIN;
    }

    while (!socket_raw_icmp_recv_ready(ent->socket)) {
        int wait_rc;
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_raw_icmp_recv_ready(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);

    return socket_raw_icmp_recv(ent->socket, buf, len, out_src_ip);
}

static int sys_udp_send_socket(fd_entry_t* ent,
                               const void* buf,
                               unsigned int len,
                               const struct sockaddr* dest_addr,
                               unsigned int addrlen) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    struct sockaddr_in sa;
    unsigned int target_ip;
    unsigned int target_port;
    unsigned int next_hop;
    unsigned int local_port;
    unsigned char packet[1488];
    int rc;

    if (!ent || !buf || socket_kind(ent->socket) != SOCKET_KIND_UDP) return -EINVAL;
    if (!cfg || !cfg->configured) return -ENETUNREACH;
    if (len > sizeof(packet) - 8u) return -EMSGSIZE;

    if (dest_addr) {
        rc = copy_user_sockaddr_in(&sa, dest_addr, addrlen);
        if (rc < 0) return rc;
        target_ip = swap_u32(sa.sin_addr.s_addr);
        target_port = swap_u16(sa.sin_port);
    } else {
        if (socket_state(ent->socket) != SOCKET_STATE_CONNECTED) return -EDESTADDRREQ;
        target_ip = socket_peer_ip(ent->socket);
        target_port = socket_peer_port(ent->socket);
    }
    if (target_ip == 0u || target_port == 0u || target_port > 0xFFFFu) {
        return -EDESTADDRREQ;
    }

    rc = socket_udp_ensure_bound(ent->socket);
    if (rc < 0) return rc;
    local_port = socket_local_port(ent->socket);
    if (local_port == 0u) return -EINVAL;

    if (copy_from_user(packet + 8u, buf, len) < 0) return -EFAULT;
    net_write_u16_be(packet, 0u, local_port);
    net_write_u16_be(packet, 2u, target_port);
    net_write_u16_be(packet, 4u, len + 8u);
    net_write_u16_be(packet, 6u, 0u);

    next_hop = net_route_next_hop(target_ip);
    if (!next_hop) return -ENETUNREACH;
    rc = ipv4_send_payload(cfg->ip,
                           target_ip,
                           next_hop,
                           IPPROTO_UDP,
                           packet,
                           len + 8u);
    return rc < 0 ? rc : (int)len;
}

static int sys_udp_recv_socket(process_t* proc,
                               fd_entry_t* ent,
                               void* buf,
                               unsigned int len,
                               unsigned int flags,
                               unsigned int* out_src_ip,
                               unsigned int* out_src_port) {
    if (!proc || !ent || !buf || socket_kind(ent->socket) != SOCKET_KIND_UDP) {
        return -EINVAL;
    }

    if (!socket_udp_recv_ready(ent->socket) &&
        ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u ||
         (flags & MSG_DONTWAIT) != 0u)) {
        return -EAGAIN;
    }

    while (!socket_udp_recv_ready(ent->socket)) {
        int wait_rc;
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_udp_recv_ready(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);

    return socket_udp_recv(ent->socket, buf, len, out_src_ip, out_src_port);
}

static int sys_recv_impl(syscall_regs_t* regs, int fd, void* buf, unsigned int len) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    int rc;

    if (!proc) return -EINVAL;
    if (len == 0u) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_kind(ent->socket) == SOCKET_KIND_NETLINK_ROUTE) {
        return sys_netlink_recv_user(proc, ent, buf, len, 0u, 0, 0);
    }
    if (socket_kind(ent->socket) == SOCKET_KIND_RAW_ICMP) {
        (void)regs;
        return sys_raw_icmp_recv_socket(proc, ent, buf, len, 0u, 0);
    }
    if (socket_kind(ent->socket) == SOCKET_KIND_UDP) {
        (void)regs;
        return sys_udp_recv_socket(proc, ent, buf, len, 0u, 0, 0);
    }
    if (socket_state(ent->socket) != SOCKET_STATE_CONNECTED &&
        socket_state(ent->socket) != SOCKET_STATE_CONNECTING) return -EINVAL;

    if (!socket_tcp_recv_ready(ent->socket) &&
        (ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
        return -EAGAIN;
    }

    while (!socket_tcp_recv_ready(ent->socket)) {
        int wait_rc;

        if (!socket_tcp_connection_established(ent->socket)) {
            if (!socket_tcp_connect_pending(ent->socket)) {
                return 0;
            }
            proc->state = PROCESS_STATE_WAITING;
            wait_rc = socket_wait(ent->socket, proc, POLLOUT);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                socket_wait_clear_process(proc);
                return wait_rc;
            }
            (void)regs;
            sys_wait_until_current_running(proc);
            socket_wait_clear_process(proc);
            continue;
        }
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_tcp_recv_ready(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        (void)regs;
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);

    rc = socket_tcp_recv(ent->socket, buf, len);
    return rc < 0 ? -ECONNRESET : rc;
}

static int sys_sendto_impl(int fd,
                           const void* buf,
                           unsigned int len,
                           unsigned int flags,
                           const struct sockaddr* dest_addr,
                           unsigned int addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    if (len == 0u) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;
    if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0u) return -EINVAL;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;

    if (socket_kind(ent->socket) == SOCKET_KIND_NETLINK_ROUTE) {
        if (dest_addr) {
            struct sockaddr_nl nl;
            if (addrlen < sizeof(nl)) return -EINVAL;
            if (!user_buf_ok((unsigned int)dest_addr, sizeof(nl))) return -EFAULT;
            if (copy_from_user(&nl, dest_addr, sizeof(nl)) < 0) return -EFAULT;
            if (nl.nl_family != AF_NETLINK) return -EINVAL;
        }
        return sys_netlink_send_user(ent, buf, len);
    }

    if (socket_kind(ent->socket) == SOCKET_KIND_TCP) {
        if (dest_addr) return -EISCONN;
        return process_fd_write(ent, (const char*)buf, len);
    }

    if (socket_kind(ent->socket) == SOCKET_KIND_RAW_ICMP) {
        struct sockaddr_in sa;
        const net_ipv4_config_t* cfg = net_ipv4_config();
        unsigned int target_ip;
        unsigned int next_hop;
        unsigned char packet[1480];
        int rc = copy_user_sockaddr_in(&sa, dest_addr, addrlen);
        if (rc < 0) return rc;
        if (!cfg || !cfg->configured) return -ENETUNREACH;
        if (len > sizeof(packet)) return -EMSGSIZE;
        if (copy_from_user(packet, buf, len) < 0) return -EFAULT;
        target_ip = swap_u32(sa.sin_addr.s_addr);
        next_hop = net_route_next_hop(target_ip);
        if (!next_hop) return -ENETUNREACH;
        rc = ipv4_send_payload(cfg->ip,
                               target_ip,
                               next_hop,
                               IPPROTO_ICMP,
                               packet,
                               len);
        return rc < 0 ? rc : (int)len;
    }

    if (socket_kind(ent->socket) == SOCKET_KIND_UDP) {
        return sys_udp_send_socket(ent, buf, len, dest_addr, addrlen);
    }

    return -EOPNOTSUPP;
}

static int sys_recvfrom_impl(syscall_regs_t* regs,
                             int fd,
                             void* buf,
                             unsigned int len,
                             unsigned int flags,
                             struct sockaddr* src_addr,
                             unsigned int* addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    if (len == 0u) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;
    if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0u) return -EINVAL;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;

    if (socket_kind(ent->socket) == SOCKET_KIND_NETLINK_ROUTE) {
        return sys_netlink_recv_user(proc, ent, buf, len, flags, src_addr, addrlen);
    }

    if (socket_kind(ent->socket) == SOCKET_KIND_TCP) {
        int rc;
        if (src_addr || addrlen) return -EISCONN;
        rc = sys_recv_impl(regs, fd, buf, len);
        return rc;
    }

    if (socket_kind(ent->socket) == SOCKET_KIND_RAW_ICMP) {
        unsigned int src_ip = 0u;
        int rc;

        rc = sys_raw_icmp_recv_socket(proc, ent, buf, len, flags, &src_ip);
        if (rc < 0) return rc;
        if (src_addr && addrlen) {
            struct sockaddr_in sa;
            unsigned int user_addrlen = 0u;
            if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
            if (user_addrlen < sizeof(sa)) return -EINVAL;
            if (!user_buf_ok((unsigned int)src_addr, sizeof(sa))) return -EFAULT;
            k_memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_addr.s_addr = swap_u32(src_ip);
            if (copy_to_user(src_addr, &sa, sizeof(sa)) < 0 ||
                write_user_u32(addrlen, sizeof(sa)) < 0) {
                return -EFAULT;
            }
        }
        return rc;
    }

    if (socket_kind(ent->socket) == SOCKET_KIND_UDP) {
        unsigned int src_ip = 0u;
        unsigned int src_port = 0u;
        int rc = sys_udp_recv_socket(proc, ent, buf, len, flags, &src_ip, &src_port);
        if (rc < 0) return rc;
        if (src_addr && addrlen) {
            struct sockaddr_in sa;
            unsigned int user_addrlen = 0u;
            if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
            if (user_addrlen < sizeof(sa)) return -EINVAL;
            if (!user_buf_ok((unsigned int)src_addr, sizeof(sa))) return -EFAULT;
            k_memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = swap_u16((unsigned short)src_port);
            sa.sin_addr.s_addr = swap_u32(src_ip);
            if (copy_to_user(src_addr, &sa, sizeof(sa)) < 0 ||
                write_user_u32(addrlen, sizeof(sa)) < 0) {
                return -EFAULT;
            }
        } else if (src_addr || addrlen) {
            return -EFAULT;
        }
        return rc;
    }

    if (src_addr || addrlen) {
        if (!src_addr || !addrlen) return -EFAULT;
        if (!user_buf_ok((unsigned int)addrlen, sizeof(*addrlen))) return -EFAULT;
    }
    return (flags & MSG_DONTWAIT) != 0u ? -EAGAIN : -EOPNOTSUPP;
}

static short sys_poll_revents_for_fd(process_t* proc, struct pollfd* pfd) {
    fd_entry_t* ent = process_fd_get(proc, pfd->fd);
    return process_fd_poll(ent, pfd->events);
}

static unsigned int sys_poll_snapshot(process_t* proc, struct pollfd* fds,
                                      unsigned int nfds) {
    unsigned int ready = 0u;

    for (unsigned int i = 0; i < nfds; i++) {
        short revents = sys_poll_revents_for_fd(proc, &fds[i]);
        fds[i].revents = revents;
        if (revents) {
            ready++;
        }
    }

    return ready;
}

static int sys_poll_register_fd_waits(process_t* proc,
                                      struct pollfd* fds,
                                      unsigned int nfds) {
    if (!proc || !fds) return -EINVAL;

    for (unsigned int i = 0; i < nfds; i++) {
        fd_entry_t* ent = process_fd_get(proc, fds[i].fd);
        int rc;

        if (!ent) continue;

        rc = process_fd_wait(ent, proc, fds[i].events);
        if (rc < 0) {
            socket_wait_clear_process(proc);
            return rc;
        }
    }

    return 0;
}

static unsigned int sys_poll_timeout_ticks(int timeout_ms) {
    if (timeout_ms <= 0) {
        return 0u;
    }

    return timer_ms_to_ticks_round_up((unsigned int)timeout_ms);
}

static int sys_poll_impl(syscall_regs_t* regs, struct pollfd* fds,
                         unsigned int nfds, int timeout) {
    process_t* proc = (process_t*)sched_current();
    unsigned int timeout_ticks;
    unsigned int deadline;
    int infinite_wait;

    if (!proc) return -EINVAL;
    if (nfds == 0u) return 0;
    if (nfds > POLL_MAX_FDS) return -EINVAL;
    if (!user_count_bytes_ok((unsigned int)fds, nfds, sizeof(struct pollfd), 0)) {
        return -EFAULT;
    }

    infinite_wait = (timeout < 0);
    timeout_ticks = infinite_wait ? 0u : sys_poll_timeout_ticks(timeout);
    deadline = infinite_wait ? 0u : (timer_get_ticks() + timeout_ticks);

    for (;;) {
        unsigned int ready = sys_poll_snapshot(proc, fds, nfds);
        if (ready != 0u) {
            socket_wait_clear_process(proc);
            wait_queue_remove_proc(proc);
            return (int)ready;
        }

        if (!infinite_wait && (int)(timer_get_ticks() - deadline) >= 0) {
            socket_wait_clear_process(proc);
            wait_queue_remove_proc(proc);
            return 0;
        }

        proc->sleep_until = infinite_wait ? 0u : deadline;
        proc->state = infinite_wait ? PROCESS_STATE_WAITING
                                    : PROCESS_STATE_SLEEPING;
        {
            int wait_rc = sys_poll_register_fd_waits(proc, fds, nfds);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                socket_wait_clear_process(proc);
                wait_queue_remove_proc(proc);
                return wait_rc;
            }
        }
        ready = sys_poll_snapshot(proc, fds, nfds);
        if (ready != 0u) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            wait_queue_remove_proc(proc);
            return (int)ready;
        }

        (void)regs;
        sys_wait_until_current_running(proc);

        socket_wait_clear_process(proc);
        wait_queue_remove_proc(proc);
    }
}

static int sys_fcntl_impl(int fd, int cmd, unsigned int arg) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;

    if (cmd == SYS_FCNTL_DUPFD) {
        return process_fd_dup(proc, fd, (int)arg, 0);
    }
    if (cmd == SYS_FCNTL_DUPFD_CLOEXEC) {
        return process_fd_dup(proc, fd, (int)arg, SYS_FD_FLAG_CLOEXEC);
    }
    if (cmd == SYS_FCNTL_GETFD) {
        return (int)process_fd_get_fd_flags(ent);
    }
    if (cmd == SYS_FCNTL_SETFD) {
        return process_fd_set_fd_flags(ent, arg);
    }
    if (cmd == SYS_FCNTL_GETFL) {
        return (int)process_fd_get_flags(ent);
    }
    if (cmd == SYS_FCNTL_SETFL) {
        return process_fd_set_flags(ent, arg);
    }

    return -EINVAL;
}

static epoll_watch_t* epoll_watches(fd_entry_t* ent, int create) {
    if (!ent || ent->kind != PROCESS_HANDLE_KIND_EPOLL) return 0;
    if (!ent->aux_frame && create) {
        ent->aux_frame = pmm_alloc_frame();
        if (!ent->aux_frame) return 0;
        k_memset(paging_phys_to_kernel_virt(ent->aux_frame), 0, PAGE_SIZE);
    }
    if (!ent->aux_frame) return 0;
    return (epoll_watch_t*)paging_phys_to_kernel_virt(ent->aux_frame);
}

static int epoll_find_watch(epoll_watch_t* watches, int fd) {
    if (!watches) return -1;
    for (unsigned int i = 0; i < EPOLL_MAX_WATCHES; i++) {
        if (watches[i].used && watches[i].fd == fd) {
            return (int)i;
        }
    }
    return -1;
}

static int sys_epoll_create_impl(int flags) {
    process_t* proc = (process_t*)sched_current();
    int fd;
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    if ((flags & ~EPOLL_CLOEXEC) != 0) return -EINVAL;

    fd = process_fd_open_special(proc, PROCESS_HANDLE_KIND_EPOLL, "epoll");
    if (fd < 0) return fd;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if ((flags & EPOLL_CLOEXEC) != 0) {
        (void)process_fd_set_fd_flags(ent, SYS_FD_FLAG_CLOEXEC);
    }
    return fd;
}

static int sys_epoll_ctl_impl(int epfd, int op, int fd,
                              struct epoll_event* user_event) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* epent;
    fd_entry_t* target;
    epoll_watch_t* watches;
    struct epoll_event event;
    int idx;

    if (!proc) return -EINVAL;
    epent = process_fd_get(proc, epfd);
    if (!epent || epent->kind != PROCESS_HANDLE_KIND_EPOLL) return -EBADF;
    target = process_fd_get(proc, fd);
    if (!target) return -EBADF;
    if (fd == epfd) return -EINVAL;

    if (op != EPOLL_CTL_DEL) {
        if (!user_event ||
            !user_buf_ok((unsigned int)user_event, sizeof(*user_event))) {
            return -EFAULT;
        }
        if (copy_from_user(&event, user_event, sizeof(event)) < 0) {
            return -EFAULT;
        }
    } else {
        k_memset(&event, 0, sizeof(event));
    }

    watches = epoll_watches(epent, op != EPOLL_CTL_DEL);
    if (!watches) return -ENOMEM;
    idx = epoll_find_watch(watches, fd);

    if (op == EPOLL_CTL_ADD) {
        if (idx >= 0) return -EEXIST;
        for (unsigned int i = 0; i < EPOLL_MAX_WATCHES; i++) {
            if (!watches[i].used) {
                watches[i].used = 1;
                watches[i].fd = fd;
                watches[i].events = event.events;
                watches[i].data_u32 = event.data.u32;
                return 0;
            }
        }
        return -ENFILE;
    }

    if (op == EPOLL_CTL_MOD) {
        if (idx < 0) return -ENOENT;
        watches[idx].events = event.events;
        watches[idx].data_u32 = event.data.u32;
        return 0;
    }

    if (op == EPOLL_CTL_DEL) {
        if (idx < 0) return -ENOENT;
        k_memset(&watches[idx], 0, sizeof(watches[idx]));
        return 0;
    }

    return -EINVAL;
}

static unsigned int epoll_snapshot(process_t* proc,
                                   epoll_watch_t* watches,
                                   struct epoll_event* events,
                                   unsigned int maxevents) {
    unsigned int ready = 0u;

    if (!proc || !watches || !events) return 0u;

    for (unsigned int i = 0; i < EPOLL_MAX_WATCHES && ready < maxevents; i++) {
        if (!watches[i].used) continue;

        fd_entry_t* ent = process_fd_get(proc, watches[i].fd);
        short revents;

        if (!ent) {
            revents = EPOLLERR;
        } else {
            revents = process_fd_poll(ent, (short)(watches[i].events & 0xFFFFu));
        }

        if (revents) {
            k_memset(&events[ready], 0, sizeof(events[ready]));
            events[ready].events = (unsigned int)revents;
            events[ready].data.u32 = watches[i].data_u32;
            ready++;
        }
    }

    return ready;
}

static int epoll_register_fd_waits(process_t* proc, epoll_watch_t* watches) {
    if (!proc) return -EINVAL;
    if (!watches) return 0;

    for (unsigned int i = 0; i < EPOLL_MAX_WATCHES; i++) {
        fd_entry_t* ent;
        int rc;

        if (!watches[i].used) continue;

        ent = process_fd_get(proc, watches[i].fd);
        if (!ent) continue;

        rc = process_fd_wait(ent,
                             proc,
                             (short)(watches[i].events & 0xFFFFu));
        if (rc < 0) {
            socket_wait_clear_process(proc);
            return rc;
        }
    }

    return 0;
}

static int sys_epoll_wait_impl(syscall_regs_t* regs,
                               int epfd,
                               struct epoll_event* events,
                               int maxevents,
                               int timeout) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* epent;
    epoll_watch_t* watches;
    unsigned int timeout_ticks;
    unsigned int timeout_deadline;
    int infinite_wait;

    if (!proc) return -EINVAL;
    if (maxevents <= 0) return -EINVAL;
    if (!events) return -EFAULT;
    if (maxevents > (int)EPOLL_MAX_WATCHES) return -EINVAL;
    if (!user_count_bytes_ok((unsigned int)events,
                             (unsigned int)maxevents,
                             sizeof(struct epoll_event),
                             0)) {
        return -EFAULT;
    }

    epent = process_fd_get(proc, epfd);
    if (!epent || epent->kind != PROCESS_HANDLE_KIND_EPOLL) return -EBADF;
    watches = epoll_watches(epent, 0);
    if (!watches) {
        if (timeout == 0) return 0;
    }

    infinite_wait = timeout < 0;
    timeout_ticks = infinite_wait ? 0u : sys_poll_timeout_ticks(timeout);
    timeout_deadline = infinite_wait ? 0u : timer_get_ticks() + timeout_ticks;

    for (;;) {
        unsigned int ready = watches ? epoll_snapshot(proc, watches, events,
                                                      (unsigned int)maxevents)
                                     : 0u;
        if (ready != 0u) {
            socket_wait_clear_process(proc);
            wait_queue_remove_proc(proc);
            return (int)ready;
        }

        if (!infinite_wait && (int)(timer_get_ticks() - timeout_deadline) >= 0) {
            socket_wait_clear_process(proc);
            wait_queue_remove_proc(proc);
            return 0;
        }

        unsigned int sleep_deadline = 0u;

        if (!infinite_wait) {
            sleep_deadline = timeout_deadline;
        }

        if (sleep_deadline != 0u) {
            proc->sleep_until = sleep_deadline;
            proc->state = PROCESS_STATE_SLEEPING;
        } else {
            proc->sleep_until = 0u;
            proc->state = PROCESS_STATE_WAITING;
        }
        {
            int wait_rc = epoll_register_fd_waits(proc, watches);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                socket_wait_clear_process(proc);
                wait_queue_remove_proc(proc);
                return wait_rc;
            }
        }
        ready = watches ? epoll_snapshot(proc, watches, events,
                                         (unsigned int)maxevents)
                        : 0u;
        if (ready != 0u) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            wait_queue_remove_proc(proc);
            return (int)ready;
        }

        (void)regs;
        sys_wait_until_current_running(proc);

        socket_wait_clear_process(proc);
        wait_queue_remove_proc(proc);
    }
}

static unsigned int timerfd_timespec_to_ticks(unsigned int sec, long nsec) {
    unsigned int hz = timer_get_hz();
    unsigned int ticks;
    unsigned int ns_per_tick;

    if (nsec < 0 || nsec >= (long)SMALLOS_NS_PER_SECOND) {
        return 0xFFFFFFFFu;
    }
    if (hz == 0u) return 0xFFFFFFFFu;
    if (sec > 0xFFFFFFFEu / hz) return 0xFFFFFFFEu;

    ticks = sec * hz;
    if (nsec > 0) {
        ns_per_tick = SMALLOS_NS_PER_SECOND / hz;
        ticks += ((unsigned int)nsec + ns_per_tick - 1u) / ns_per_tick;
    }
    return ticks;
}

static int sys_timerfd_create_impl(int clock_id, int flags) {
    process_t* proc = (process_t*)sched_current();
    int fd;
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) return -EINVAL;
    if ((flags & ~(SYS_FD_FLAG_NONBLOCK | SOCK_CLOEXEC)) != 0) return -EINVAL;

    fd = process_fd_open_special(proc, PROCESS_HANDLE_KIND_TIMERFD, "timerfd");
    if (fd < 0) return fd;
    ent = process_fd_get(proc, fd);
    if (ent) {
        (void)process_fd_set_flags(ent, flags);
        if ((flags & SOCK_CLOEXEC) != 0) {
            (void)process_fd_set_fd_flags(ent, SYS_FD_FLAG_CLOEXEC);
        }
    }
    return fd;
}

static int sys_clock_gettime_impl(int clock_id, struct user_timespec* ts) {
    struct user_timespec out;
    unsigned int ticks;
    unsigned int rem;

    if (!ts) return -EFAULT;
    if (clock_id != CLOCK_REALTIME && clock_id != CLOCK_MONOTONIC) return -EINVAL;

    ticks = timer_get_ticks();
    rem = ticks % timer_get_hz();
    out.tv_sec = (clock_id == CLOCK_REALTIME)
               ? timer_get_realtime_seconds()
               : timer_get_seconds();
    out.tv_nsec = (long)(rem * (SMALLOS_NS_PER_SECOND / timer_get_hz()));
    return copy_to_user(ts, &out, sizeof(out));
}

static int sys_clock_settime_impl(int clock_id, const struct user_timespec* ts) {
    struct user_timespec in;

    if (!ts) return -EFAULT;
    if (clock_id != CLOCK_REALTIME) return -EINVAL;
    if (copy_from_user(&in, ts, sizeof(in)) < 0) return -EFAULT;
    if (in.tv_nsec < 0 || in.tv_nsec >= (long)SMALLOS_NS_PER_SECOND) return -EINVAL;

    timer_set_realtime_seconds(in.tv_sec);
    return 0;
}

static int sys_ntp_sync_impl(unsigned int server_ip, struct user_timespec* out_ts) {
    struct user_timespec out;
    unsigned int unix_time;

    __asm__ __volatile__("sti");
    if (!ntp_sync(server_ip, &unix_time)) {
        return -ETIMEDOUT;
    }

    timer_set_realtime_seconds(unix_time);
    if (out_ts) {
        out.tv_sec = unix_time;
        out.tv_nsec = 0;
        return copy_to_user(out_ts, &out, sizeof(out));
    }
    return 0;
}

static int sys_timerfd_settime_impl(int fd,
                                    int flags,
                                    const struct user_itimerspec* new_value,
                                    struct user_itimerspec* old_value) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct user_itimerspec spec;
    unsigned int first_ticks;
    unsigned int interval_ticks;

    if (!proc) return -EINVAL;
    if (flags != 0) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent || ent->kind != PROCESS_HANDLE_KIND_TIMERFD) return -EBADF;
    if (!new_value ||
        !user_buf_ok((unsigned int)new_value, sizeof(*new_value))) {
        return -EFAULT;
    }
    if (old_value &&
        !user_buf_ok((unsigned int)old_value, sizeof(*old_value))) {
        return -EFAULT;
    }

    if (old_value) {
        struct user_itimerspec zero;
        k_memset(&zero, 0, sizeof(zero));
        if (copy_to_user(old_value, &zero, sizeof(zero)) < 0) {
            return -EFAULT;
        }
    }

    if (copy_from_user(&spec, new_value, sizeof(spec)) < 0) {
        return -EFAULT;
    }
    first_ticks = timerfd_timespec_to_ticks(spec.it_value.tv_sec,
                                            spec.it_value.tv_nsec);
    interval_ticks = timerfd_timespec_to_ticks(spec.it_interval.tv_sec,
                                               spec.it_interval.tv_nsec);
    if (first_ticks == 0xFFFFFFFFu || interval_ticks == 0xFFFFFFFFu) {
        return -EINVAL;
    }

    ent->timer_interval = interval_ticks;
    ent->timer_deadline = first_ticks ? timer_get_ticks() + first_ticks : 0u;
    return 0;
}

static int sys_signalfd_impl(int fd, const void* mask, int flags) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    int out_fd = fd;
    unsigned int kernel_mask = 0u;
    int rc;

    if (!proc) return -EINVAL;
    if (mask && !user_buf_ok((unsigned int)mask, sizeof(unsigned int))) {
        return -EFAULT;
    }
    if ((flags & ~(SYS_FD_FLAG_NONBLOCK | SOCK_CLOEXEC)) != 0) return -EINVAL;

    if (fd < 0) {
        out_fd = process_fd_open_special(proc, PROCESS_HANDLE_KIND_SIGNALFD, "signalfd");
        if (out_fd < 0) return out_fd;
    }

    ent = process_fd_get(proc, out_fd);
    if (!ent || ent->kind != PROCESS_HANDLE_KIND_SIGNALFD) return -EBADF;
    if (mask) {
        if (copy_from_user(&kernel_mask, mask, sizeof(kernel_mask)) < 0) {
            return -EFAULT;
        }
        rc = process_fd_set_signalfd_mask(ent, kernel_mask);
        if (rc < 0) return rc;
    } else if (fd < 0) {
        rc = process_fd_set_signalfd_mask(ent, kernel_mask);
        if (rc < 0) return rc;
    }
    (void)process_fd_set_flags(ent, flags);
    if ((flags & SOCK_CLOEXEC) != 0) {
        (void)process_fd_set_fd_flags(ent, SYS_FD_FLAG_CLOEXEC);
    }
    return out_fd;
}

static int sys_shutdown_impl(int fd, int how) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    return socket_shutdown_tcp(ent->socket, how);
}

static int sys_getpeername_impl(int fd, struct sockaddr* addr, unsigned int* addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_state(ent->socket) != SOCKET_STATE_CONNECTED) return -EINVAL;
    if (!addr || !addrlen) return -EFAULT;
    {
        unsigned int user_addrlen = 0;
        if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
        if (user_addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    }
    if (!user_buf_ok((unsigned int)addr, sizeof(struct sockaddr_in))) return -EFAULT;

    k_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = swap_u16((unsigned short)socket_peer_port(ent->socket));
    sa.sin_addr.s_addr = swap_u32(socket_peer_ip(ent->socket));
    if (copy_to_user(addr, &sa, sizeof(sa)) < 0 ||
        write_user_u32(addrlen, sizeof(sa)) < 0) {
        return -EFAULT;
    }
    return 0;
}

static int sys_mkdir_impl(const char* path, unsigned int mode) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;

    if (path_rc < 0) return path_rc;
    if (!proc) return -EINVAL;
    if (path_on_pseudo_mount(kpath)) return -EACCES;
    perm_rc = check_parent_permission(proc, kpath, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    if (!vfs_mkdir(kpath)) return -EIO;
    (void)vfs_chmod(kpath, (u16)mode_after_umask(proc, SYS_MODE_IFDIR, mode));
    (void)vfs_chown(kpath, (u16)proc->euid, (u16)proc->egid);
    return 0;
}

static int sys_mkdirat_impl(int dirfd, const char* path, unsigned int mode) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_at_resolved(kpath, sizeof(kpath), dirfd, path);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;

    if (path_rc < 0) return path_rc;
    if (!proc) return -EINVAL;
    if (path_on_pseudo_mount(kpath)) return -EACCES;
    perm_rc = check_parent_permission(proc, kpath, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    if (!vfs_mkdir(kpath)) return -EIO;
    (void)vfs_chmod(kpath, (u16)mode_after_umask(proc, SYS_MODE_IFDIR, mode));
    (void)vfs_chown(kpath, (u16)proc->euid, (u16)proc->egid);
    return 0;
}

static int sys_rmdir_impl(const char* path) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;

    if (path_rc < 0) return path_rc;
    if (!proc) return -EINVAL;
    if (path_is_mount_target(kpath)) return -EBUSY;
    if (path_on_pseudo_mount(kpath)) return -EACCES;
    perm_rc = check_parent_permission(proc, kpath, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    return vfs_rmdir(kpath) ? 0 : path_lookup_errno(kpath);
}

static int virtual_dirent_at(const char* path,
                             unsigned int index,
                             char* out_name,
                             unsigned int out_name_size,
                             unsigned int* out_size,
                             int* out_is_dir) {
    static const char* const proc_entries[] = {
        "meminfo", "uptime", "stat", "mounts", "filesystems", "net", "self"
    };
    static const char* const proc_net_entries[] = {
        "dev", "route"
    };
    static const char* const pid_entries[] = {
        "stat", "status", "cmdline", "comm"
    };
    static const char* const dev_entries[] = {
        "null", "zero", "tty", "console", "fd"
    };
    static const char* const fd_entries[] = {
        "0", "1", "2"
    };
    const char* name = 0;
    int is_dir = 0;
    unsigned int size = 0;
    char translated[PROCESS_FD_NAME_MAX];

    if (!path || !out_name || out_name_size == 0u) return 0;
    path = virtual_effective_path(path, translated, sizeof(translated));

    if (path_eq(path, "proc")) {
        unsigned int proc_count = sizeof(proc_entries) / sizeof(proc_entries[0]);
        if (index < proc_count) {
            name = proc_entries[index];
            is_dir = path_eq(name, "self") || path_eq(name, "net");
        } else {
            process_t* procs[SYS_PROCINFO_MAX];
            int count = sched_snapshot_all(procs, SYS_PROCINFO_MAX);
            unsigned int proc_index = index - proc_count;
            if (proc_index >= (unsigned int)count || !procs[proc_index]) return 0;
            {
                static char pid_buf[16];
                unsigned int pos = 0;
                pid_buf[0] = '\0';
                vbuf_put_uint(pid_buf, sizeof(pid_buf), &pos, procs[proc_index]->pid);
                name = pid_buf;
                is_dir = 1;
            }
        }
    } else if (path_eq(path, "dev")) {
        if (index >= sizeof(dev_entries) / sizeof(dev_entries[0])) return 0;
        name = dev_entries[index];
        is_dir = path_eq(name, "fd");
    } else if (path_eq(path, "proc/net")) {
        if (index >= sizeof(proc_net_entries) / sizeof(proc_net_entries[0])) return 0;
        name = proc_net_entries[index];
    } else if (path_eq(path, "dev/fd")) {
        if (index >= sizeof(fd_entries) / sizeof(fd_entries[0])) return 0;
        name = fd_entries[index];
    } else {
        process_t* proc = 0;
        const char* leaf = 0;
        if (!virtual_proc_pid_path(path, &proc, &leaf) || !proc || !leaf || leaf[0] != '\0') {
            return 0;
        }
        if (index >= sizeof(pid_entries) / sizeof(pid_entries[0])) return 0;
        name = pid_entries[index];
    }

    if (!name || (unsigned int)k_strlen(name) + 1u > out_name_size) return 0;
    k_memcpy(out_name, name, (k_size_t)k_strlen(name) + 1u);
    if (!is_dir) {
        char full[PROCESS_FD_NAME_MAX];
        unsigned int pos = 0;
        vbuf_puts(full, sizeof(full), &pos, path);
        vbuf_putc(full, sizeof(full), &pos, '/');
        vbuf_puts(full, sizeof(full), &pos, name);
        (void)virtual_path_exists(full, &size, &is_dir, 0);
    }
    if (out_size) *out_size = size;
    if (out_is_dir) *out_is_dir = is_dir;
    return 1;
}

static int sys_dirlist_impl(const char* path, unsigned int index, uapi_dirent_t* out) {
    char kpath[PROCESS_FD_NAME_MAX];
    char name[UAPI_DIRENT_NAME_MAX];
    unsigned int size = 0;
    int is_dir = 0;
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    if (!out) return -EFAULT;
    if (!user_buf_ok((unsigned int)out, sizeof(*out))) return -EFAULT;
    {
        process_t* proc = (process_t*)sched_current();
        int perm_rc = check_path_permission(proc, kpath, SYS_PERM_R | SYS_PERM_X, 0);
        if (perm_rc < 0) return perm_rc;
    }
    if (!virtual_dirent_at(kpath, index, name, sizeof(name), &size, &is_dir) &&
        !vfs_dirent_at(kpath, index, name, sizeof(name), &size, &is_dir)) {
        return 0;
    }
    {
        uapi_dirent_t kout;
        k_memset(&kout, 0, sizeof(kout));
        k_memcpy(kout.d_name, name, k_strlen(name) + 1u);
        kout.d_size = size;
        kout.d_is_dir = is_dir;
        if (copy_to_user(out, &kout, sizeof(kout)) < 0) return -EFAULT;
    }
    return 1;
}

static int sys_dirlist_batch_impl(const char* path,
                                  unsigned int index,
                                  uapi_dirent_t* out,
                                  unsigned int max_count) {
    char kpath[PROCESS_FD_NAME_MAX];
    ext2_dirent_info_t* entries;
    unsigned int entries_frame;
    unsigned int entries_frames;
    unsigned int entries_bytes;
    unsigned int count = 0;
    int path_rc;
    int rc = 0;

    if (max_count == 0) return 0;
    if (max_count > DIRLIST_BATCH_MAX) max_count = DIRLIST_BATCH_MAX;
    if (!out) return -EFAULT;
    if (!user_buf_ok((unsigned int)out, sizeof(*out) * max_count)) return -EFAULT;

    path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    {
        process_t* proc = (process_t*)sched_current();
        int perm_rc = check_path_permission(proc, kpath, SYS_PERM_R | SYS_PERM_X, 0);
        if (perm_rc < 0) return perm_rc;
    }

    if (virtual_path_is_dir(kpath)) {
        while (count < max_count) {
            char name[UAPI_DIRENT_NAME_MAX];
            unsigned int size = 0;
            int is_dir = 0;
            if (!virtual_dirent_at(kpath, index + count, name, sizeof(name), &size, &is_dir)) {
                break;
            }
            k_memset(&out[count], 0, sizeof(out[count]));
            k_memcpy(out[count].d_name, name, k_strlen(name) + 1u);
            out[count].d_size = size;
            out[count].d_is_dir = is_dir;
            count++;
        }
        return (int)count;
    }

    entries_bytes = sizeof(*entries) * max_count;
    entries_frames = (entries_bytes + PMM_FRAME_SIZE - 1u) / PMM_FRAME_SIZE;
    entries_frame = pmm_alloc_contiguous_frames(entries_frames);
    if (!entries_frame) return -ENOMEM;
    entries = (ext2_dirent_info_t*)paging_phys_to_kernel_virt(entries_frame);

    if (!vfs_dirents_read(kpath, index, entries, max_count, &count)) {
        pmm_free_contiguous_frames(entries_frame, entries_frames);
        return 0;
    }

    for (unsigned int i = 0; i < count; i++) {
        uapi_dirent_t kout;
        k_memset(&kout, 0, sizeof(kout));
        k_memcpy(kout.d_name, entries[i].name, sizeof(kout.d_name));
        kout.d_size = entries[i].size;
        kout.d_is_dir = entries[i].is_dir;
        if (copy_to_user(&out[i], &kout, sizeof(kout)) < 0) {
            rc = -EFAULT;
            break;
        }
    }

    pmm_free_contiguous_frames(entries_frame, entries_frames);
    return rc < 0 ? rc : (int)count;
}

static int sys_setsockopt_impl(int fd, int level, int optname) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    (void)level;
    (void)optname;
    return 0;
}

static int sys_getsockname_impl(int fd, struct sockaddr* addr, unsigned int* addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (!addr || !addrlen) return -EFAULT;
    if (socket_kind(ent->socket) == SOCKET_KIND_NETLINK_ROUTE) {
        struct sockaddr_nl nl;
        unsigned int user_addrlen = 0;
        if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
        if (user_addrlen < sizeof(nl)) return -EINVAL;
        if (!user_buf_ok((unsigned int)addr, sizeof(nl))) return -EFAULT;
        k_memset(&nl, 0, sizeof(nl));
        nl.nl_family = AF_NETLINK;
        nl.nl_pid = socket_netlink_pid(ent->socket);
        nl.nl_groups = socket_netlink_groups(ent->socket);
        if (copy_to_user(addr, &nl, sizeof(nl)) < 0 ||
            write_user_u32(addrlen, sizeof(nl)) < 0) {
            return -EFAULT;
        }
        return 0;
    }
    {
        unsigned int user_addrlen = 0;
        if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
        if (user_addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    }
    if (!user_buf_ok((unsigned int)addr, sizeof(struct sockaddr_in))) return -EFAULT;

    k_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = swap_u16((unsigned short)socket_local_port(ent->socket));
    sa.sin_addr.s_addr = swap_u32(socket_local_ip(ent->socket));
    if (copy_to_user(addr, &sa, sizeof(sa)) < 0 ||
        write_user_u32(addrlen, sizeof(sa)) < 0) {
        return -EFAULT;
    }
    return 0;
}

static int net_ifreq_name_is_eth0(const char* name) {
    return name && k_strcmp(name, "eth0");
}

static void net_set_sockaddr_in(struct sockaddr* out, unsigned int ip) {
    struct sockaddr_in* in = (struct sockaddr_in*)out;
    k_memset(out, 0, sizeof(*out));
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = swap_u32(ip);
}

static unsigned int net_sockaddr_ip(const struct sockaddr* addr) {
    const struct sockaddr_in* in = (const struct sockaddr_in*)addr;
    if (!addr || addr->sa_family != AF_INET) return 0u;
    return swap_u32(in->sin_addr.s_addr);
}

static unsigned int net_route_next_hop(unsigned int target_ip) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    if (!cfg || !cfg->configured) return 0u;
    if (cfg->netmask != 0u &&
        (target_ip & cfg->netmask) == (cfg->ip & cfg->netmask)) {
        return target_ip;
    }
    return cfg->gateway ? cfg->gateway : target_ip;
}

static void net_write_u16_be(unsigned char* buf, unsigned int off, unsigned int value) {
    buf[off] = (unsigned char)((value >> 8) & 0xFFu);
    buf[off + 1u] = (unsigned char)(value & 0xFFu);
}

static void net_fill_eth0_ifreq(struct ifreq* ifr, unsigned int request) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    const u8* mac = nic_mac();
    unsigned int flags = IFF_BROADCAST | IFF_MULTICAST;

    k_memset(ifr, 0, sizeof(*ifr));
    k_memcpy(ifr->ifr_name, "eth0", 5u);
    if (cfg && cfg->configured) flags |= IFF_UP;
    if (nic_link_up()) flags |= IFF_RUNNING;

    switch (request) {
    case SIOCGIFFLAGS:
        ifr->ifr_flags = (short)flags;
        break;
    case SIOCGIFADDR:
        net_set_sockaddr_in(&ifr->ifr_addr, cfg && cfg->configured ? cfg->ip : 0u);
        break;
    case SIOCGIFDSTADDR:
        net_set_sockaddr_in(&ifr->ifr_dstaddr, cfg ? cfg->gateway : 0u);
        break;
    case SIOCGIFBRDADDR:
        net_set_sockaddr_in(&ifr->ifr_broadaddr,
                            cfg && cfg->configured
                              ? ((cfg->ip & cfg->netmask) | (~cfg->netmask))
                              : 0u);
        break;
    case SIOCGIFNETMASK:
        net_set_sockaddr_in(&ifr->ifr_netmask,
                            cfg && cfg->configured ? cfg->netmask : 0u);
        break;
    case SIOCGIFHWADDR:
        k_memset(&ifr->ifr_hwaddr, 0, sizeof(ifr->ifr_hwaddr));
        ifr->ifr_hwaddr.sa_family = ARPHRD_ETHER;
        if (mac) k_memcpy(ifr->ifr_hwaddr.sa_data, mac, 6u);
        break;
    case SIOCGIFMTU:
        ifr->ifr_mtu = 1500;
        break;
    case SIOCGIFINDEX:
        ifr->ifr_ifindex = 1;
        break;
    case SIOCGIFMETRIC:
        ifr->ifr_metric = 0;
        break;
    case SIOCGIFTXQLEN:
        ifr->ifr_qlen = 1000;
        break;
    case SIOCGIFNAME:
        break;
    default:
        break;
    }
}

static int net_apply_ifreq_setter(unsigned int request, const struct ifreq* ifr) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    unsigned int ip = cfg ? cfg->ip : 0u;
    unsigned int netmask = cfg && cfg->netmask ? cfg->netmask : 0xFFFFFF00u;
    unsigned int gateway = cfg ? cfg->gateway : 0u;
    unsigned int dns = cfg ? cfg->dns : 0u;
    unsigned int dhcp_server = cfg ? cfg->dhcp_server : 0u;
    unsigned int lease_seconds = cfg ? cfg->lease_seconds : 0u;

    switch (request) {
    case SIOCSIFFLAGS:
    case SIOCSIFBRDADDR:
    case SIOCSIFDSTADDR:
    case SIOCSIFMETRIC:
    case SIOCSIFMTU:
    case SIOCSIFTXQLEN:
        return 0;
    case SIOCSIFADDR:
        ip = net_sockaddr_ip(&ifr->ifr_addr);
        if (ip == 0u) return -EINVAL;
        break;
    case SIOCSIFNETMASK:
        netmask = net_sockaddr_ip(&ifr->ifr_netmask);
        if (netmask == 0u) return -EINVAL;
        break;
    default:
        return -ENOTTY;
    }

    net_ipv4_configure(ip, netmask, gateway, dns, dhcp_server, lease_seconds);
    return 0;
}

static int net_ioctl_ifconf(void* argp) {
    struct ifconf ifc;
    struct ifreq ifr;

    if (!argp) return -EFAULT;
    if (!user_buf_ok((unsigned int)argp, sizeof(ifc))) return -EFAULT;
    if (copy_from_user(&ifc, argp, sizeof(ifc)) < 0) return -EFAULT;

    if (ifc.ifc_len >= (int)sizeof(ifr) && ifc.ifc_buf) {
        if (!user_buf_ok((unsigned int)ifc.ifc_buf, sizeof(ifr))) return -EFAULT;
        net_fill_eth0_ifreq(&ifr, SIOCGIFADDR);
        if (copy_to_user(ifc.ifc_buf, &ifr, sizeof(ifr)) < 0) return -EFAULT;
    }
    ifc.ifc_len = (int)sizeof(ifr);
    if (copy_to_user(argp, &ifc, sizeof(ifc)) < 0) return -EFAULT;
    return 0;
}

static int net_ioctl_ifreq(unsigned int request, void* argp) {
    struct ifreq ifr;

    if (!argp) return -EFAULT;
    if (!user_buf_ok((unsigned int)argp, sizeof(ifr))) return -EFAULT;
    if (copy_from_user(&ifr, argp, sizeof(ifr)) < 0) return -EFAULT;

    if (request == SIOCGIFNAME) {
        if (ifr.ifr_ifindex != 1) return -ENODEV;
        net_fill_eth0_ifreq(&ifr, request);
        return copy_to_user(argp, &ifr, sizeof(ifr)) < 0 ? -EFAULT : 0;
    }

    if (!net_ifreq_name_is_eth0(ifr.ifr_name)) return -ENODEV;
    switch (request) {
    case SIOCGIFFLAGS:
    case SIOCGIFADDR:
    case SIOCGIFDSTADDR:
    case SIOCGIFBRDADDR:
    case SIOCGIFNETMASK:
    case SIOCGIFHWADDR:
    case SIOCGIFMTU:
    case SIOCGIFMETRIC:
    case SIOCGIFTXQLEN:
    case SIOCGIFINDEX:
        net_fill_eth0_ifreq(&ifr, request);
        return copy_to_user(argp, &ifr, sizeof(ifr)) < 0 ? -EFAULT : 0;
    case SIOCSIFFLAGS:
    case SIOCSIFADDR:
    case SIOCSIFDSTADDR:
    case SIOCSIFBRDADDR:
    case SIOCSIFNETMASK:
    case SIOCSIFMETRIC:
    case SIOCSIFMTU:
    case SIOCSIFTXQLEN:
        return net_apply_ifreq_setter(request, &ifr);
    default:
        return -ENOTTY;
    }
}

static int net_ioctl_route(unsigned int request, void* argp) {
    struct rtentry rt;
    const net_ipv4_config_t* cfg;
    unsigned int dst;
    unsigned int gateway;
    unsigned int netmask;

    if (!argp) return -EFAULT;
    if (!user_buf_ok((unsigned int)argp, sizeof(rt))) return -EFAULT;
    if (copy_from_user(&rt, argp, sizeof(rt)) < 0) return -EFAULT;

    cfg = net_ipv4_config();
    if (!cfg || !cfg->configured) return -ENETUNREACH;

    dst = net_sockaddr_ip(&rt.rt_dst);
    gateway = net_sockaddr_ip(&rt.rt_gateway);
    netmask = net_sockaddr_ip(&rt.rt_genmask);

    if (request == SIOCADDRT) {
        if ((rt.rt_flags & RTF_GATEWAY) == 0u) return 0;
        if (dst != 0u || gateway == 0u) return -EINVAL;
        net_ipv4_configure(cfg->ip,
                           cfg->netmask,
                           gateway,
                           cfg->dns,
                           cfg->dhcp_server,
                           cfg->lease_seconds);
        return 0;
    }

    if (request == SIOCDELRT) {
        if (dst != 0u || (netmask != 0u && netmask != 0xFFFFFFFFu)) return -EINVAL;
        net_ipv4_configure(cfg->ip,
                           cfg->netmask,
                           0u,
                           cfg->dns,
                           cfg->dhcp_server,
                           cfg->lease_seconds);
        return 0;
    }
    return -ENOTTY;
}

static int sys_net_ioctl_impl(int fd, unsigned int request, void* argp) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;

    if (request == SIOCGIFCONF) return net_ioctl_ifconf(argp);
    if (request == SIOCADDRT || request == SIOCDELRT) {
        return net_ioctl_route(request, argp);
    }
    if ((request >= SIOCGIFNAME && request <= SIOCGIFHWADDR) ||
        request == SIOCGIFTXQLEN || request == SIOCSIFTXQLEN ||
        request == SIOCGIFINDEX) {
        return net_ioctl_ifreq(request, argp);
    }
    return -ENOTTY;
}

static int sys_writefd_impl(int fd, const char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (ent->kind == PROCESS_HANDLE_KIND_SOCKET &&
        ent->socket &&
        socket_kind(ent->socket) == SOCKET_KIND_NETLINK_ROUTE) {
        return sys_netlink_send_user(ent, buf, len);
    }
    return process_fd_write(ent, buf, len);
}

static int sys_lseek_impl(int fd, int offset, int whence) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    return process_fd_seek(ent, offset, whence);
}

static int sys_fsync_impl(int fd) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (!ent->writable) return 0;
    return process_fd_flush(ent) ? 0 : -EIO;
}

static int sys_unlink_impl(const char* path) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;

    if (path_rc < 0) return path_rc;
    if (!proc) return -EINVAL;
    if (path_is_mount_target(kpath)) return -EBUSY;
    if (path_on_pseudo_mount(kpath)) return -EACCES;
    if (vfs_is_dir(kpath)) return -EISDIR;
    perm_rc = check_parent_permission(proc, kpath, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    return vfs_unlink(kpath) ? 0 : path_lookup_errno(kpath);
}

static int sys_unlinkat_impl(int dirfd, const char* path, unsigned int flags) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_at_resolved(kpath, sizeof(kpath), dirfd, path);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;

    if ((flags & ~SYS_AT_REMOVEDIR) != 0u) return -EINVAL;
    if (path_rc < 0) return path_rc;
    if (!proc) return -EINVAL;
    if (flags & SYS_AT_REMOVEDIR) {
        if (path_is_mount_target(kpath)) return -EBUSY;
        if (path_on_pseudo_mount(kpath)) return -EACCES;
        perm_rc = check_parent_permission(proc, kpath, SYS_PERM_W | SYS_PERM_X);
        if (perm_rc < 0) return perm_rc;
        return vfs_rmdir(kpath) ? 0 : path_lookup_errno(kpath);
    }
    if (path_is_mount_target(kpath)) return -EBUSY;
    if (path_on_pseudo_mount(kpath)) return -EACCES;
    if (vfs_is_dir(kpath)) return -EISDIR;
    perm_rc = check_parent_permission(proc, kpath, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    return vfs_unlink(kpath) ? 0 : path_lookup_errno(kpath);
}

static int sys_link_impl(const char* oldpath, const char* newpath) {
    char kold[PROCESS_FD_NAME_MAX];
    char knew[PROCESS_FD_NAME_MAX];
    sys_stat_info_t info;
    int old_rc = copy_user_path_resolved(kold, sizeof(kold), oldpath);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;

    if (old_rc < 0) return old_rc;
    int new_rc = copy_user_path_resolved(knew, sizeof(knew), newpath);
    if (new_rc < 0) return new_rc;
    if (!proc) return -EINVAL;
    if (path_on_pseudo_mount(kold) || path_on_pseudo_mount(knew)) return -EACCES;
    perm_rc = check_path_permission(proc, kold, 0, 0);
    if (perm_rc < 0) return perm_rc;
    perm_rc = check_parent_permission(proc, knew, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    if (vfs_is_dir(kold)) return -EPERM;
    if (vfs_link(kold, knew)) return 0;
    if (vfs_lstat_info(knew, &info)) return -EEXIST;
    return path_lookup_errno(kold);
}

static int sys_linkat_impl(int olddirfd,
                           const char* oldpath,
                           int newdirfd,
                           const char* newpath,
                           unsigned int flags) {
    char kold[PROCESS_FD_NAME_MAX];
    char knew[PROCESS_FD_NAME_MAX];
    sys_stat_info_t info;
    int old_rc = copy_user_path_at_resolved(kold, sizeof(kold), olddirfd, oldpath);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;

    if ((flags & ~SYS_AT_SYMLINK_FOLLOW) != 0u) return -EINVAL;
    if (old_rc < 0) return old_rc;
    {
        int new_rc = copy_user_path_at_resolved(knew, sizeof(knew), newdirfd, newpath);
        if (new_rc < 0) return new_rc;
    }
    if (!proc) return -EINVAL;
    if (path_on_pseudo_mount(kold) || path_on_pseudo_mount(knew)) return -EACCES;
    perm_rc = check_path_permission(proc, kold, 0, 0);
    if (perm_rc < 0) return perm_rc;
    perm_rc = check_parent_permission(proc, knew, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    if (vfs_is_dir(kold)) return -EPERM;
    if (vfs_link(kold, knew)) return 0;
    if (vfs_lstat_info(knew, &info)) return -EEXIST;
    return path_lookup_errno(kold);
}

static int sys_symlink_impl(const char* target, const char* linkpath) {
    char ktarget[PROCESS_FD_NAME_MAX];
    char klink[PROCESS_FD_NAME_MAX];
    int target_rc = copy_user_cstr(ktarget, sizeof(ktarget), target);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;

    if (target_rc < 0) return target_rc;
    if (target_rc <= 1) return -EINVAL;
    int link_rc = copy_user_path_resolved(klink, sizeof(klink), linkpath);
    if (link_rc < 0) return link_rc;
    if (!proc) return -EINVAL;
    if (path_on_pseudo_mount(klink)) return -EACCES;
    perm_rc = check_parent_permission(proc, klink, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    if (vfs_symlink(ktarget, klink)) return 0;
    {
        sys_stat_info_t info;
        if (vfs_lstat_info(klink, &info)) return -EEXIST;
    }
    return path_lookup_errno(klink);
}

static int sys_symlinkat_impl(const char* target, int newdirfd, const char* linkpath) {
    char ktarget[PROCESS_FD_NAME_MAX];
    char klink[PROCESS_FD_NAME_MAX];
    int target_rc = copy_user_cstr(ktarget, sizeof(ktarget), target);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;

    if (target_rc < 0) return target_rc;
    if (target_rc <= 1) return -EINVAL;
    {
        int link_rc = copy_user_path_at_resolved(klink, sizeof(klink), newdirfd, linkpath);
        if (link_rc < 0) return link_rc;
    }
    if (!proc) return -EINVAL;
    if (path_on_pseudo_mount(klink)) return -EACCES;
    perm_rc = check_parent_permission(proc, klink, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    if (vfs_symlink(ktarget, klink)) return 0;
    {
        sys_stat_info_t info;
        if (vfs_lstat_info(klink, &info)) return -EEXIST;
    }
    return path_lookup_errno(klink);
}

static int sys_readlink_impl(const char* path, char* out, unsigned int out_size) {
    char kpath[PROCESS_FD_NAME_MAX];
    char target[PROCESS_FD_NAME_MAX];
    u32 len = 0;
    u32 copied;
    int path_rc;

    if (!out || out_size == 0u) return -EINVAL;
    path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    {
        process_t* proc = (process_t*)sched_current();
        int perm_rc = check_path_prefix_execute(proc, kpath);
        if (perm_rc < 0) return perm_rc;
    }
    if (!vfs_readlink(kpath, target, sizeof(target), &len)) {
        sys_stat_info_t info;
        if (vfs_lstat_info(kpath, &info)) return -EINVAL;
        return path_lookup_errno(kpath);
    }
    copied = len;
    if (copied > out_size) copied = out_size;
    if (copy_to_user(out, target, copied) < 0) return -EFAULT;
    return (int)copied;
}

static int sys_readlinkat_impl(int dirfd, const char* path, char* out, unsigned int out_size) {
    char kpath[PROCESS_FD_NAME_MAX];
    char target[PROCESS_FD_NAME_MAX];
    u32 len = 0;
    u32 copied;
    int path_rc;

    if (!out || out_size == 0u) return -EINVAL;
    path_rc = copy_user_path_at_resolved(kpath, sizeof(kpath), dirfd, path);
    if (path_rc < 0) return path_rc;
    {
        process_t* proc = (process_t*)sched_current();
        int perm_rc = check_path_prefix_execute(proc, kpath);
        if (perm_rc < 0) return perm_rc;
    }
    if (!vfs_readlink(kpath, target, sizeof(target), &len)) {
        sys_stat_info_t info;
        if (vfs_lstat_info(kpath, &info)) return -EINVAL;
        return path_lookup_errno(kpath);
    }
    copied = len;
    if (copied > out_size) copied = out_size;
    if (copy_to_user(out, target, copied) < 0) return -EFAULT;
    return (int)copied;
}

static int sys_rename_impl(const char* src, const char* dst) {
    char ksrc[PROCESS_FD_NAME_MAX];
    char kdst[PROCESS_FD_NAME_MAX];
    int src_rc = copy_user_path_resolved(ksrc, sizeof(ksrc), src);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;

    if (src_rc < 0) return src_rc;
    int dst_rc = copy_user_path_resolved(kdst, sizeof(kdst), dst);
    if (dst_rc < 0) return dst_rc;
    if (!proc) return -EINVAL;
    if (path_is_mount_target(ksrc) || path_is_mount_target(kdst)) return -EBUSY;
    if (path_on_pseudo_mount(ksrc) || path_on_pseudo_mount(kdst)) return -EACCES;
    perm_rc = check_parent_permission(proc, ksrc, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    perm_rc = check_parent_permission(proc, kdst, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    return vfs_rename(ksrc, kdst) ? 0 : path_lookup_errno(ksrc);
}

static int sys_renameat_impl(int olddirfd,
                             const char* oldpath,
                             int newdirfd,
                             const char* newpath) {
    char ksrc[PROCESS_FD_NAME_MAX];
    char kdst[PROCESS_FD_NAME_MAX];
    int src_rc = copy_user_path_at_resolved(ksrc, sizeof(ksrc), olddirfd, oldpath);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;

    if (src_rc < 0) return src_rc;
    {
        int dst_rc = copy_user_path_at_resolved(kdst, sizeof(kdst), newdirfd, newpath);
        if (dst_rc < 0) return dst_rc;
    }
    if (!proc) return -EINVAL;
    if (path_is_mount_target(ksrc) || path_is_mount_target(kdst)) return -EBUSY;
    if (path_on_pseudo_mount(ksrc) || path_on_pseudo_mount(kdst)) return -EACCES;
    perm_rc = check_parent_permission(proc, ksrc, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    perm_rc = check_parent_permission(proc, kdst, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    return vfs_rename(ksrc, kdst) ? 0 : path_lookup_errno(ksrc);
}

static int sys_chmod_impl(const char* path, unsigned int mode) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    process_t* proc = (process_t*)sched_current();
    sys_stat_info_t info;
    int perm_rc;

    if (path_rc < 0) return path_rc;
    if (!proc) return -EINVAL;
    if (path_on_pseudo_mount(kpath)) return -EACCES;
    perm_rc = check_path_permission(proc, kpath, 0, &info);
    if (perm_rc < 0) return perm_rc;
    if (!process_is_root(proc) && proc->euid != info.uid) return -EPERM;
    return vfs_chmod(kpath, (u16)mode) ? 0 : path_lookup_errno(kpath);
}

static int sys_chown_impl(const char* path, unsigned int uid, unsigned int gid) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;

    if (path_rc < 0) return path_rc;
    if (!proc) return -EINVAL;
    if (path_on_pseudo_mount(kpath)) return -EACCES;
    if (!process_is_root(proc)) return -EPERM;
    perm_rc = check_path_permission(proc, kpath, 0, 0);
    if (perm_rc < 0) return perm_rc;
    return vfs_chown(kpath, (u16)uid, (u16)gid) ? 0 : path_lookup_errno(kpath);
}

static int sys_utimens_impl(const char* path, const struct user_timespec* times) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    return sys_utimens_kpath_impl(kpath, times, 0);
}

static int sys_utimens_kpath_impl(const char* kpath,
                                  const struct user_timespec* times,
                                  int nofollow) {
    struct user_timespec in[2];
    u32 atime;
    u32 mtime;
    process_t* proc = (process_t*)sched_current();
    sys_stat_info_t info;
    int perm_rc;

    if (!proc) return -EINVAL;
    if (path_on_pseudo_mount(kpath)) return -EACCES;
    if (nofollow) {
        perm_rc = check_path_prefix_execute(proc, kpath);
        if (perm_rc < 0) return perm_rc;
        if (!vfs_lstat_info(kpath, &info)) return path_lookup_errno(kpath);
    } else {
        perm_rc = check_path_permission(proc, kpath, 0, &info);
        if (perm_rc < 0) return perm_rc;
    }
    if (!process_is_root(proc) && proc->euid != info.uid &&
        !permission_allows(proc, &info, SYS_PERM_W)) {
        return -EACCES;
    }
    if (times) {
        if (copy_from_user(in, times, sizeof(in)) < 0) return -EFAULT;
        if (in[0].tv_nsec < 0 || in[0].tv_nsec >= (long)SMALLOS_NS_PER_SECOND ||
            in[1].tv_nsec < 0 || in[1].tv_nsec >= (long)SMALLOS_NS_PER_SECOND) {
            return -EINVAL;
        }
        atime = in[0].tv_sec;
        mtime = in[1].tv_sec;
    } else {
        atime = timer_get_realtime_seconds();
        mtime = atime;
    }
    if (nofollow) {
        return vfs_lutimes(kpath, atime, mtime) ? 0 : path_lookup_errno(kpath);
    }
    return vfs_utimes(kpath, atime, mtime) ? 0 : path_lookup_errno(kpath);
}

static int sys_utimensat_impl(int dirfd,
                              const char* path,
                              const struct user_timespec* times,
                              unsigned int flags) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc;

    if ((flags & ~SYS_AT_SYMLINK_NOFOLLOW) != 0u) return -EINVAL;
    path_rc = copy_user_path_at_resolved(kpath, sizeof(kpath), dirfd, path);
    if (path_rc < 0) return path_rc;
    return sys_utimens_kpath_impl(kpath, times,
                                  (flags & SYS_AT_SYMLINK_NOFOLLOW) != 0u);
}

static int sys_mknod_impl(const char* path, unsigned int mode, unsigned int dev) {
    char kpath[PROCESS_FD_NAME_MAX];
    sys_stat_info_t info;
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;
    unsigned int final_mode;

    if (path_rc < 0) return path_rc;
    if (!proc) return -EINVAL;
    if (path_on_pseudo_mount(kpath)) return -EACCES;
    perm_rc = check_parent_permission(proc, kpath, SYS_PERM_W | SYS_PERM_X);
    if (perm_rc < 0) return perm_rc;
    final_mode = mode_after_umask(proc, mode & SYS_MODE_IFMT, mode);
    if (vfs_mknod(kpath, (u16)final_mode, dev)) {
        (void)vfs_chown(kpath, (u16)proc->euid, (u16)proc->egid);
        return 0;
    }
    if (vfs_lstat_info(kpath, &info)) return -EEXIST;
    return path_lookup_errno(kpath);
}

static int sys_ftruncate_impl(int fd, unsigned int size) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (ent->kind == PROCESS_HANDLE_KIND_FILE) {
        int perm_rc = check_path_permission(proc, ent->name, SYS_PERM_W, 0);
        if (perm_rc < 0) return perm_rc;
    }
    return vfs_file_truncate_fd(ent, size);
}

static int sys_fchmod_impl(int fd, unsigned int mode) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (ent->kind != PROCESS_HANDLE_KIND_FILE) return -EBADF;
    {
        sys_stat_info_t info;
        int perm_rc = check_path_permission(proc, ent->name, 0, &info);
        if (perm_rc < 0) return perm_rc;
        if (!process_is_root(proc) && proc->euid != info.uid) return -EPERM;
    }
    return vfs_chmod(ent->name, (u16)mode) ? 0 : -EIO;
}

static int sys_fchown_impl(int fd, unsigned int uid, unsigned int gid) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (ent->kind != PROCESS_HANDLE_KIND_FILE) return -EBADF;
    if (!process_is_root(proc)) return -EPERM;
    {
        int perm_rc = check_path_permission(proc, ent->name, 0, 0);
        if (perm_rc < 0) return perm_rc;
    }
    return vfs_chown(ent->name, (u16)uid, (u16)gid) ? 0 : -EIO;
}

static int sys_futimens_impl(int fd, const struct user_timespec* times) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct user_timespec in[2];
    u32 atime;
    u32 mtime;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (ent->kind != PROCESS_HANDLE_KIND_FILE) return -EBADF;
    {
        sys_stat_info_t info;
        int perm_rc = check_path_permission(proc, ent->name, 0, &info);
        if (perm_rc < 0) return perm_rc;
        if (!process_is_root(proc) && proc->euid != info.uid &&
            !permission_allows(proc, &info, SYS_PERM_W)) {
            return -EACCES;
        }
    }
    if (times) {
        if (copy_from_user(in, times, sizeof(in)) < 0) return -EFAULT;
        if (in[0].tv_nsec < 0 || in[0].tv_nsec >= (long)SMALLOS_NS_PER_SECOND ||
            in[1].tv_nsec < 0 || in[1].tv_nsec >= (long)SMALLOS_NS_PER_SECOND) {
            return -EINVAL;
        }
        atime = in[0].tv_sec;
        mtime = in[1].tv_sec;
    } else {
        atime = timer_get_realtime_seconds();
        mtime = atime;
    }
    return vfs_utimes(ent->name, atime, mtime) ? 0 : -EIO;
}

static unsigned int sys_getuid_impl(void) {
    process_t* proc = (process_t*)sched_current();
    return proc ? proc->uid : 0u;
}

static unsigned int sys_geteuid_impl(void) {
    process_t* proc = (process_t*)sched_current();
    return proc ? proc->euid : 0u;
}

static unsigned int sys_getgid_impl(void) {
    process_t* proc = (process_t*)sched_current();
    return proc ? proc->gid : 0u;
}

static unsigned int sys_getegid_impl(void) {
    process_t* proc = (process_t*)sched_current();
    return proc ? proc->egid : 0u;
}

static int sys_setuid_impl(unsigned int uid) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    if (!process_is_root(proc) && uid != proc->uid && uid != proc->euid) {
        return -EPERM;
    }
    if (process_is_root(proc)) {
        proc->uid = uid;
    }
    proc->euid = uid;
    return 0;
}

static int sys_setgid_impl(unsigned int gid) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    if (!process_is_root(proc) && gid != proc->gid && gid != proc->egid) {
        return -EPERM;
    }
    if (process_is_root(proc)) {
        proc->gid = gid;
        proc->supp_gid = gid;
        proc->supp_gid_count = 1u;
    }
    proc->egid = gid;
    return 0;
}

static unsigned int sys_umask_impl(unsigned int mask) {
    process_t* proc = (process_t*)sched_current();
    unsigned int old = proc ? proc->umask : 0022u;
    if (proc) proc->umask = mask & 0777u;
    return old;
}

static int sys_stat_impl(const char* path, unsigned int* out_size, int* out_is_dir) {
    char kpath[PROCESS_FD_NAME_MAX];
    u32 size = 0;
    int is_dir = 0;
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    {
        process_t* proc = (process_t*)sched_current();
        int perm_rc = check_path_prefix_execute(proc, kpath);
        if (perm_rc < 0) return perm_rc;
    }

    if (!virtual_path_exists(kpath, &size, &is_dir, 0) &&
        !vfs_stat(kpath, &size, &is_dir)) {
        return path_lookup_errno(kpath);
    }

    if (out_size) {
        if (write_user_u32(out_size, size) < 0) return -EFAULT;
    }
    if (out_is_dir) {
        if (copy_to_user(out_is_dir, &is_dir, sizeof(is_dir)) < 0) return -EFAULT;
    }
    return 0;
}

static int sys_fstat_impl(int fd, unsigned int* out_size, int* out_is_dir) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    u32 size = 0;
    int is_dir = 0;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;

    if (ent->kind == PROCESS_HANDLE_KIND_FILE) {
        int rc = vfs_file_stat_fd(ent, &size, &is_dir);
        if (rc < 0) return rc;
    } else {
        size = ent->is_dir ? 0u : ent->size;
        is_dir = ent->is_dir ? 1 : 0;
    }

    if (out_size) {
        if (write_user_u32(out_size, size) < 0) return -EFAULT;
    }
    if (out_is_dir) {
        if (copy_to_user(out_is_dir, &is_dir, sizeof(is_dir)) < 0) return -EFAULT;
    }
    return 0;
}

static int sys_stat_full_impl(const char* path, sys_stat_info_t* out) {
    char kpath[PROCESS_FD_NAME_MAX];
    sys_stat_info_t info;
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);

    if (path_rc < 0) return path_rc;
    if (!out) return -EFAULT;
    {
        process_t* proc = (process_t*)sched_current();
        int perm_rc = check_path_prefix_execute(proc, kpath);
        if (perm_rc < 0) return perm_rc;
    }
    if (!virtual_stat_info(kpath, &info) && !vfs_stat_info(kpath, &info)) {
        return path_lookup_errno(kpath);
    }
    if (copy_to_user(out, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_lstat_full_impl(const char* path, sys_stat_info_t* out) {
    char kpath[PROCESS_FD_NAME_MAX];
    sys_stat_info_t info;
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);

    if (path_rc < 0) return path_rc;
    if (!out) return -EFAULT;
    {
        process_t* proc = (process_t*)sched_current();
        int perm_rc = check_path_prefix_execute(proc, kpath);
        if (perm_rc < 0) return perm_rc;
    }
    if (!virtual_stat_info(kpath, &info) && !vfs_lstat_info(kpath, &info)) {
        return path_lookup_errno(kpath);
    }
    if (copy_to_user(out, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_fstatat_full_impl(int dirfd,
                                 const char* path,
                                 sys_stat_info_t* out,
                                 unsigned int flags) {
    char kpath[PROCESS_FD_NAME_MAX];
    sys_stat_info_t info;
    int path_rc;

    if ((flags & ~SYS_AT_SYMLINK_NOFOLLOW) != 0u) return -EINVAL;
    path_rc = copy_user_path_at_resolved(kpath, sizeof(kpath), dirfd, path);
    if (path_rc < 0) return path_rc;
    if (!out) return -EFAULT;
    {
        process_t* proc = (process_t*)sched_current();
        int perm_rc = check_path_prefix_execute(proc, kpath);
        if (perm_rc < 0) return perm_rc;
    }
    if (flags & SYS_AT_SYMLINK_NOFOLLOW) {
        if (!virtual_stat_info(kpath, &info) && !vfs_lstat_info(kpath, &info)) {
            return path_lookup_errno(kpath);
        }
    } else {
        if (!virtual_stat_info(kpath, &info) && !vfs_stat_info(kpath, &info)) {
            return path_lookup_errno(kpath);
        }
    }
    if (copy_to_user(out, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_fstat_full_impl(int fd, sys_stat_info_t* out) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    sys_stat_info_t info;

    if (!proc) return -EINVAL;
    if (!out) return -EFAULT;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;

    if (ent->kind == PROCESS_HANDLE_KIND_FILE) {
        int rc = vfs_file_stat_info_fd(ent, &info);
        if (rc < 0) return rc;
    } else if (ent->kind == PROCESS_HANDLE_KIND_VIRTUAL) {
        if (!virtual_stat_info(ent->name, &info)) {
            k_memset(&info, 0, sizeof(info));
            info.mode = 0020000u | 0600u;
            info.nlink = 1u;
            info.blksize = PAGE_SIZE;
            info.size = ent->size;
        }
    } else {
        k_memset(&info, 0, sizeof(info));
        info.mode = 0020000u | 0600u;
        info.nlink = 1u;
        info.blksize = 4096u;
        info.size = ent->size;
    }

    if (copy_to_user(out, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_terminal_size_impl(unsigned int* out_rows, unsigned int* out_cols) {
    process_t* proc;
    fd_entry_t* stdin_ent;
    unsigned int rows = 0;
    unsigned int cols = 0;

    if (!out_rows || !out_cols) return -EFAULT;
    proc = (process_t*)sched_current();
    stdin_ent = proc ? process_fd_get(proc, 0) : 0;
    if (process_fd_terminal_size(stdin_ent, &rows, &cols) < 0) {
        rows = (unsigned int)terminal_rows();
        cols = (unsigned int)terminal_cols();
    }
    if (write_user_u32(out_rows, rows) < 0) return -EFAULT;
    if (write_user_u32(out_cols, cols) < 0) return -EFAULT;
    return 0;
}

static int sys_tcgetattr_impl(int fd, sys_termios_t* out) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    sys_termios_t ktio;
    int rc;

    if (!out) return -EFAULT;
    if (!user_buf_ok((unsigned int)out, sizeof(*out))) return -EFAULT;
    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    rc = process_fd_terminal_getattr(ent, &ktio);
    if (rc < 0) return rc;
    if (copy_to_user(out, &ktio, sizeof(ktio)) < 0) return -EFAULT;
    return 0;
}

static int sys_tcsetattr_impl(int fd, const sys_termios_t* in) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    sys_termios_t ktio;

    if (!in) return -EFAULT;
    if (!user_buf_ok((unsigned int)in, sizeof(*in))) return -EFAULT;
    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (copy_from_user(&ktio, in, sizeof(ktio)) < 0) return -EFAULT;
    return process_fd_terminal_setattr(ent, &ktio);
}

static int sys_tty_ioctl_impl(int fd, unsigned int request, void* arg) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (!process_fd_is_terminal(ent)) return -ENOTTY;

    if (request == SYS_IOCTL_TIOCGWINSZ) {
        sys_winsize_t ws;
        unsigned int rows = 0;
        unsigned int cols = 0;
        int rc;

        if (!arg || !user_buf_ok((unsigned int)arg, sizeof(ws))) return -EFAULT;
        rc = process_fd_terminal_size(ent, &rows, &cols);
        if (rc < 0) return rc;
        k_memset(&ws, 0, sizeof(ws));
        ws.ws_row = (unsigned short)rows;
        ws.ws_col = (unsigned short)cols;
        if (copy_to_user(arg, &ws, sizeof(ws)) < 0) return -EFAULT;
        return 0;
    }

    if (request == SYS_IOCTL_TIOCGPGRP) {
        u32 pgid = 0;
        int rc;

        if (!arg || !user_buf_ok((unsigned int)arg, sizeof(int))) return -EFAULT;
        rc = process_fd_terminal_get_pgrp(ent, proc->pgid, &pgid);
        if (rc < 0) return rc;
        if (copy_to_user(arg, &pgid, sizeof(int)) < 0) return -EFAULT;
        return 0;
    }

    if (request == SYS_IOCTL_TIOCSPGRP) {
        u32 pgid = 0;
        process_t* group_proc;
        int rc;

        if (!arg || !user_buf_ok((unsigned int)arg, sizeof(int))) return -EFAULT;
        if (copy_from_user(&pgid, arg, sizeof(int)) < 0) return -EFAULT;
        group_proc = process_find_by_pgid(pgid);
        if (!group_proc) return -ESRCH;
        if (group_proc->sid != proc->sid) return -EPERM;
        rc = process_fd_terminal_set_pgrp(ent, pgid);
        return rc;
    }

    return -ENOTTY;
}

static int resource_limit_value(process_t* proc,
                                int resource,
                                unsigned int* cur,
                                unsigned int* max) {
    if (!proc || !cur || !max) return -EINVAL;

    switch (resource) {
        case SYS_RLIMIT_CPU:
            *cur = SYS_RLIM_INFINITY;
            *max = SYS_RLIM_INFINITY;
            return 0;
        case SYS_RLIMIT_DATA:
            *cur = (USER_STACK_TOP - USER_STACK_SIZE) - USER_HEAP_BASE;
            *max = *cur;
            return 0;
        case SYS_RLIMIT_STACK:
            *cur = USER_STACK_SIZE;
            *max = USER_STACK_SIZE;
            return 0;
        case SYS_RLIMIT_AS:
            *cur = USER_STACK_TOP - USER_CODE_BASE;
            *max = *cur;
            return 0;
        case SYS_RLIMIT_NOFILE:
            *cur = proc->fd_limit;
            *max = PROCESS_FD_LIMIT_HARD;
            return 0;
        default:
            return -EINVAL;
    }
}

static int sys_getrlimit_impl(int resource, sys_rlimit_t* out) {
    process_t* proc = (process_t*)sched_current();
    sys_rlimit_t lim;
    int rc;

    if (!out) return -EFAULT;
    if (!user_buf_ok((unsigned int)out, sizeof(*out))) return -EFAULT;
    if (!proc) return -EINVAL;

    rc = resource_limit_value(proc, resource, &lim.rlim_cur, &lim.rlim_max);
    if (rc < 0) return rc;
    if (copy_to_user(out, &lim, sizeof(lim)) < 0) return -EFAULT;
    return 0;
}

static int sys_setrlimit_impl(int resource, const sys_rlimit_t* in) {
    process_t* proc = (process_t*)sched_current();
    sys_rlimit_t lim;
    unsigned int cur;
    unsigned int max;
    int rc;

    if (!in) return -EFAULT;
    if (!user_buf_ok((unsigned int)in, sizeof(*in))) return -EFAULT;
    if (!proc) return -EINVAL;
    if (copy_from_user(&lim, in, sizeof(lim)) < 0) return -EFAULT;

    rc = resource_limit_value(proc, resource, &cur, &max);
    if (rc < 0) return rc;
    if (lim.rlim_cur > lim.rlim_max) return -EINVAL;
    if (lim.rlim_max > max || lim.rlim_cur > max) return -EINVAL;

    if (resource == SYS_RLIMIT_NOFILE) {
        if (lim.rlim_cur < proc->fd_capacity) return -EINVAL;
        proc->fd_limit = lim.rlim_cur;
        return 0;
    }

    if (lim.rlim_cur != cur || lim.rlim_max != max) return -EINVAL;
    return 0;
}

static int sys_getrusage_impl(int who, sys_rusage_t* out) {
    process_t* proc = (process_t*)sched_current();
    sys_rusage_t usage;
    unsigned int ticks;

    if (!out) return -EFAULT;
    if (!user_buf_ok((unsigned int)out, sizeof(*out))) return -EFAULT;
    if (!proc) return -EINVAL;
    if (who != SYS_RUSAGE_SELF && who != SYS_RUSAGE_CHILDREN) return -EINVAL;

    k_memset(&usage, 0, sizeof(usage));
    if (who == SYS_RUSAGE_SELF) {
        ticks = proc->cpu_ticks;
        usage.ru_utime.tv_sec = (long)(ticks / 1000u);
        usage.ru_utime.tv_usec = (long)((ticks % 1000u) * 1000u);
        usage.ru_maxrss = (long)(process_ram_bytes(proc) / 1024u);
    } else {
        ticks = proc->child_cpu_ticks;
        usage.ru_utime.tv_sec = (long)(ticks / 1000u);
        usage.ru_utime.tv_usec = (long)((ticks % 1000u) * 1000u);
        usage.ru_nvcsw = (long)proc->child_wait_count;
    }

    if (copy_to_user(out, &usage, sizeof(usage)) < 0) return -EFAULT;
    return 0;
}

static void statfs_fill_pseudo(const kernel_mount_t* mnt, sys_statfs_t* out) {
    k_memset(out, 0, sizeof(*out));
    out->f_type = mnt ? mnt->magic : 0;
    out->f_bsize = PAGE_SIZE;
    out->f_frsize = PAGE_SIZE;
    out->f_files = 1024;
    out->f_ffree = 1024;
    out->f_namelen = 255;
    out->f_flags = mnt ? (long)mnt->flags : 0;
}

static int statfs_fill_ext2(const kernel_mount_t* mnt, sys_statfs_t* out) {
    ext2_fsinfo_t info;

    if (!ext2_fsinfo(&info)) return -EIO;
    k_memset(out, 0, sizeof(*out));
    out->f_type = mnt ? mnt->magic : SYS_STATFS_EXT2_MAGIC;
    out->f_bsize = info.cluster_bytes ? (long)info.cluster_bytes : 1024L;
    out->f_frsize = out->f_bsize;
    out->f_blocks = info.cluster_bytes
                        ? (long)info.total_clusters
                        : (long)(info.total_bytes / 1024u);
    out->f_bfree = info.cluster_bytes
                       ? (long)info.free_clusters
                       : (long)(info.free_bytes / 1024u);
    out->f_bavail = out->f_bfree;
    out->f_files = 1024;
    out->f_ffree = 512;
    out->f_namelen = 255;
    out->f_flags = mnt ? (long)mnt->flags : 0;
    return 0;
}

static int statfs_fill_for_mount(const kernel_mount_t* mnt, sys_statfs_t* out) {
    if (!mnt || !out) return -EINVAL;
    if (mnt->magic == SYS_STATFS_EXT2_MAGIC) {
        return statfs_fill_ext2(mnt, out);
    }
    statfs_fill_pseudo(mnt, out);
    return 0;
}

static int sys_statfs_kpath_impl(const char* kpath, sys_statfs_t* out) {
    sys_stat_info_t info;
    sys_statfs_t stfs;
    const kernel_mount_t* mnt;
    process_t* proc = (process_t*)sched_current();
    int perm_rc;
    int rc;

    if (!kpath || !out) return -EFAULT;
    if (!user_buf_ok((unsigned int)out, sizeof(*out))) return -EFAULT;

    perm_rc = check_path_prefix_execute(proc, kpath);
    if (perm_rc < 0) return perm_rc;
    if (!virtual_stat_info(kpath, &info) && !vfs_stat_info(kpath, &info)) {
        return path_lookup_errno(kpath);
    }

    mnt = mount_find_for_path(kpath);
    rc = statfs_fill_for_mount(mnt, &stfs);
    if (rc < 0) return rc;
    if (copy_to_user(out, &stfs, sizeof(stfs)) < 0) return -EFAULT;
    return 0;
}

static int sys_statfs_impl(const char* path, sys_statfs_t* out) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    return sys_statfs_kpath_impl(kpath, out);
}

static int sys_fstatfs_impl(int fd, sys_statfs_t* out) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    sys_statfs_t stfs;
    const kernel_mount_t* mnt;
    int rc;

    if (!out) return -EFAULT;
    if (!user_buf_ok((unsigned int)out, sizeof(*out))) return -EFAULT;
    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;

    if (ent->name[0] != '\0') {
        mnt = mount_find_for_path(ent->name);
    } else if (ent->kind == PROCESS_HANDLE_KIND_CONSOLE ||
               ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER ||
               ent->kind == PROCESS_HANDLE_KIND_PTY_SLAVE) {
        mnt = mount_find_for_path("dev");
    } else {
        mnt = mount_find_for_path("");
    }

    rc = statfs_fill_for_mount(mnt, &stfs);
    if (rc < 0) return rc;
    if (copy_to_user(out, &stfs, sizeof(stfs)) < 0) return -EFAULT;
    return 0;
}

static int mount_fstype_supported(const char* fstype) {
    if (!fstype || fstype[0] == '\0') return 0;
    return path_eq(fstype, "ext2") || path_eq(fstype, "proc") ||
           path_eq(fstype, "devtmpfs");
}

static long mount_magic_for_fstype(const char* fstype) {
    if (path_eq(fstype, "ext2")) return SYS_STATFS_EXT2_MAGIC;
    if (path_eq(fstype, "proc")) return SYS_STATFS_PROC_MAGIC;
    if (path_eq(fstype, "devtmpfs")) return SYS_STATFS_DEV_MAGIC;
    return 0;
}

static int mount_pseudo_for_fstype(const char* fstype) {
    return path_eq(fstype, "proc") || path_eq(fstype, "devtmpfs");
}

static int mount_target_busy(const kernel_mount_t* mnt) {
    process_t* procs[SCHED_MAX_PROCS];
    int count;

    if (!mnt || !mnt->used || !mnt->target[0]) return 1;
    count = sched_snapshot_all(procs, SCHED_MAX_PROCS);
    for (int i = 0; i < count; i++) {
        process_t* proc = procs[i];
        if (!proc) continue;
        if (mount_path_matches(proc->cwd, mnt->target)) return 1;
        for (unsigned int fd = 0; fd < proc->fd_capacity; fd++) {
            fd_entry_t* ent = &proc->fds[fd];
            if (!ent->valid || ent->name[0] == '\0') continue;
            if (mount_path_matches(ent->name, mnt->target)) return 1;
        }
    }
    return 0;
}

static int sys_mount_impl(const char* source,
                          const char* target,
                          const char* fstype,
                          unsigned int flags,
                          const void* data) {
    char ksource[PROCESS_FD_NAME_MAX];
    char ktarget[PROCESS_FD_NAME_MAX];
    char kfstype[32];
    sys_stat_info_t info;
    kernel_mount_t* slot;
    int rc;

    (void)data;
    if (!target || !fstype) return -EFAULT;
    if ((flags & SYS_MOUNT_MS_MGC_MSK) == SYS_MOUNT_MS_MGC_VAL) {
        flags &= ~SYS_MOUNT_MS_MGC_MSK;
    }
    if ((flags & SYS_MOUNT_ACTION_FLAGS) != 0u) return -ENOSYS;
    if ((flags & ~SYS_MOUNT_SUPPORTED_FLAGS) != 0u) return -EINVAL;
    if (source) {
        rc = copy_user_cstr(ksource, sizeof(ksource), source);
        if (rc < 0) return rc;
        if (ksource[0] == '\0') return -EINVAL;
    }
    rc = copy_user_path_resolved(ktarget, sizeof(ktarget), target);
    if (rc < 0) return rc;
    rc = copy_user_cstr(kfstype, sizeof(kfstype), fstype);
    if (rc < 0) return rc;

    if (!mount_fstype_supported(kfstype)) return -EINVAL;
    if (mount_find_exact(ktarget)) return -EBUSY;
    if (!vfs_stat_info(ktarget, &info)) {
        return path_lookup_errno(ktarget);
    }
    if (!info.is_dir) return -ENOTDIR;
    if (path_eq(kfstype, "ext2")) return -ENOSYS;
    if (!mount_pseudo_for_fstype(kfstype)) return -EINVAL;

    slot = mount_alloc_slot();
    if (!slot) return -ENFILE;
    k_memset(slot, 0, sizeof(*slot));
    slot->used = 1u;
    slot->dynamic = 1u;
    k_strncpy(slot->source, source ? ksource : kfstype, sizeof(slot->source));
    k_strncpy(slot->target, ktarget, sizeof(slot->target));
    k_strncpy(slot->fstype, kfstype, sizeof(slot->fstype));
    k_strncpy(slot->options, (flags & SYS_MOUNT_MS_RDONLY) ? "ro" : "rw",
              sizeof(slot->options));
    slot->flags = flags;
    slot->magic = mount_magic_for_fstype(kfstype);
    slot->pseudo = 1u;
    return 0;
}

static int sys_umount2_impl(const char* target, unsigned int flags) {
    char ktarget[PROCESS_FD_NAME_MAX];
    kernel_mount_t* mnt;
    int rc;

    if (!target) return -EFAULT;
    if ((flags & ~(SYS_UMOUNT_MNT_FORCE | SYS_UMOUNT_MNT_DETACH)) != 0u) {
        return -EINVAL;
    }
    rc = copy_user_path_resolved(ktarget, sizeof(ktarget), target);
    if (rc < 0) return rc;

    mnt = mount_find_exact_mutable(ktarget);
    if (!mnt) return -EINVAL;
    if (!mnt->dynamic) return -EBUSY;
    if (mount_target_busy(mnt)) return -EBUSY;
    k_memset(mnt, 0, sizeof(*mnt));
    return 0;
}

static int sys_display_info_impl(sys_display_info_t* out_info) {
    display_info_t info;

    if (!out_info) return -EFAULT;
    if (!user_buf_ok((unsigned int)out_info, sizeof(*out_info))) return -EFAULT;
    if (!display_get_info(&info)) return -EIO;

    {
        sys_display_info_t user_info;
        user_info.width = info.width;
        user_info.height = info.height;
        user_info.pitch = info.pitch;
        user_info.bpp = info.bpp;
        user_info.format = info.format;
        if (copy_to_user(out_info, &user_info, sizeof(user_info)) < 0) return -EFAULT;
    }
    return 0;
}

static int sys_display_acquire_impl(void) {
    process_t* proc = (process_t*)sched_current();
    if (!display_acquire(proc)) {
        return -EIO;
    }
    process_set_display_input_owner(proc, 1);
    keyboard_buf_clear();
    input_clear_events();
    return 0;
}

static int sys_display_release_impl(void) {
    process_t* proc = (process_t*)sched_current();
    process_set_display_input_owner(proc, 0);
    keyboard_buf_clear();
    input_clear_events();
    display_release(proc);
    return 0;
}

static int sys_display_fill_impl(const sys_display_fill_rect_t* user_req) {
    sys_display_fill_rect_t req;

    if (!user_req) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;
    if (req.w == 0 || req.h == 0) return 0;
    if (!display_fill((process_t*)sched_current(), req.x, req.y, req.w, req.h, req.color)) return -EIO;
    return 0;
}

static int sys_display_blit_impl(const sys_display_blit_rect_t* user_req) {
    sys_display_blit_rect_t req;
    unsigned int bytes;

    if (!user_req) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;
    if (req.w == 0 || req.h == 0) return 0;
    if (!req.pixels) return -EFAULT;
    if (req.h > 0xFFFFFFFFu / req.w) return -EOVERFLOW;
    if (req.w * req.h > 0xFFFFFFFFu / sizeof(unsigned int)) return -EOVERFLOW;
    bytes = req.w * req.h * sizeof(unsigned int);
    if (!user_buf_ok((unsigned int)req.pixels, bytes)) return -EFAULT;
    if (!display_blit((process_t*)sched_current(), req.x, req.y, req.w, req.h, req.pixels)) return -EIO;
    return 0;
}

static int sys_display_blit_stride_impl(const sys_display_blit_stride_rect_t* user_req) {
    sys_display_blit_stride_rect_t req;
    unsigned int span_pixels;
    unsigned int bytes;

    if (!user_req) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;
    if (req.w == 0 || req.h == 0) return 0;
    if (!req.pixels) return -EFAULT;
    if (req.pitch_pixels < req.w) return -EINVAL;
    if (req.h - 1u > 0xFFFFFFFFu / req.pitch_pixels) return -EOVERFLOW;
    span_pixels = (req.h - 1u) * req.pitch_pixels;
    if (span_pixels > 0xFFFFFFFFu - req.w) return -EOVERFLOW;
    span_pixels += req.w;
    if (span_pixels > 0xFFFFFFFFu / sizeof(unsigned int)) return -EOVERFLOW;
    bytes = span_pixels * sizeof(unsigned int);
    if (!user_buf_ok((unsigned int)req.pixels, bytes)) return -EFAULT;
    if (!display_blit_stride((process_t*)sched_current(),
                             req.x, req.y, req.w, req.h,
                             req.pitch_pixels, req.pixels)) {
        return -EIO;
    }
    return 0;
}

static int sys_display_map_impl(sys_display_map_info_t* out_info) {
    sys_display_map_info_t info;

    if (!out_info) return -EFAULT;
    if (!user_buf_ok((unsigned int)out_info, sizeof(*out_info))) return -EFAULT;
    if (!display_map((process_t*)sched_current(), &info)) return -EIO;
    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_display_present_page_impl(sys_display_present_page_t* user_req) {
    sys_display_present_page_t req;

    if (!user_req) return -EFAULT;
    if (!user_buf_ok((unsigned int)user_req, sizeof(*user_req))) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;
    if (!display_present_page((process_t*)sched_current(), &req)) return -EIO;
    if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
    return 0;
}

static int sys_mouse_read_impl(sys_mouse_state_t* out_state) {
    sys_mouse_state_t state;

    if (!out_state) return -EFAULT;
    if (!mouse_read_state(&state)) return -EIO;
    if (copy_to_user(out_state, &state, sizeof(state)) < 0) return -EFAULT;
    return 0;
}

static int sys_mouse_debug_impl(sys_mousedebug_t* out_info) {
    mouse_debug_state_t debug;
    sys_mousedebug_t info;

    if (!out_info) return -EFAULT;
    mouse_debug_snapshot(&debug);
    info.irq_count = debug.irq_count;
    info.byte_count = debug.byte_count;
    info.aux_status_count = debug.aux_status_count;
    info.packet_count = debug.packet_count;
    info.vmware_packet_count = debug.vmware_packet_count;
    info.sync_drop_count = debug.sync_drop_count;
    info.overflow_drop_count = debug.overflow_drop_count;
    info.vmware_enabled = debug.vmware_enabled;
    info.packet_size = debug.packet_size;
    info.device_id = debug.device_id;
    info.ready = debug.ready;
    info.init_step = debug.init_step;
    info.init_fail = debug.init_fail;
    info.config_before = debug.config_before;
    info.config_after = debug.config_after;
    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_usbinfo_impl(sys_usbinfo_t* out_info) {
    usb_debug_state_t debug;
    sys_usbinfo_t info;

    if (!out_info) return -EFAULT;
    usb_debug_snapshot(&debug);
    info.controller_count = debug.controller_count;
    info.uhci_count = debug.uhci_count;
    info.ohci_count = debug.ohci_count;
    info.ehci_count = debug.ehci_count;
    info.xhci_count = debug.xhci_count;
    info.powered_port_count = debug.powered_port_count;
    info.keyboard_active = debug.keyboard_active;
    info.keyboard_port = debug.keyboard_port;
    info.keyboard_endpoint = debug.keyboard_endpoint;
    info.keyboard_packet_size = debug.keyboard_packet_size;
    info.keyboard_interval = debug.keyboard_interval;
    info.keyboard_poll_count = debug.keyboard_poll_count;
    info.keyboard_report_count = debug.keyboard_report_count;
    info.keyboard_fail_count = debug.keyboard_fail_count;
    info.keyboard_last_cc = debug.keyboard_last_cc;
    info.mouse_active = debug.mouse_active;
    info.mouse_port = debug.mouse_port;
    info.mouse_endpoint = debug.mouse_endpoint;
    info.mouse_packet_size = debug.mouse_packet_size;
    info.mouse_interval = debug.mouse_interval;
    info.mouse_poll_count = debug.mouse_poll_count;
    info.mouse_report_count = debug.mouse_report_count;
    info.mouse_fail_count = debug.mouse_fail_count;
    info.mouse_last_cc = debug.mouse_last_cc;
    info.service_active = debug.service_active;
    info.storage_active = debug.storage_active;
    info.storage_port = debug.storage_port;
    info.last_bar = debug.last_bar;
    info.last_ports = debug.last_ports;
    info.last_port_status0 = debug.last_port_status0;
    info.last_port_status1 = debug.last_port_status1;
    info.last_bus = debug.last_bus;
    info.last_slot = debug.last_slot;
    info.last_func = debug.last_func;
    info.last_prog_if = debug.last_prog_if;
    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_usb_diag_op_impl(unsigned int op, unsigned int arg) {
    switch (op) {
        case SYS_USB_DIAG_OP_PORT_SNAPSHOT: {
            sys_usb_port_snapshot_t snapshot;
            if (!arg) return -EFAULT;
            usb_port_snapshot(&snapshot);
            if (copy_to_user((sys_usb_port_snapshot_t*)arg,
                             &snapshot,
                             sizeof(snapshot)) < 0) {
                return -EFAULT;
            }
            return 0;
        }
        case SYS_USB_DIAG_OP_PORTS:
            usb_dump_ports();
            return 0;
        case SYS_USB_DIAG_OP_DIAG:
            usb_diag();
            return 0;
        case SYS_USB_DIAG_OP_PEEK:
            usb_peek_port(arg);
            return 0;
        case SYS_USB_DIAG_OP_POWER:
            return (int)usb_power_ohci_ports();
        default:
            return -EINVAL;
    }
}

static int sys_usb_mouse_op_impl(unsigned int op, unsigned int port) {
    switch (op) {
        case SYS_USB_MOUSE_OP_OPEN:
            return usb_mouse_open_port_quiet(port) ? 1 : 0;
        case SYS_USB_MOUSE_OP_POLL:
            return usb_mouse_poll_once();
        case SYS_USB_MOUSE_OP_CLOSE:
            usb_mouse_close();
            return 0;
        default:
            return -EINVAL;
    }
}

static int sys_input_read_impl(syscall_regs_t* regs,
                               sys_input_event_t* out_events,
                               unsigned int max_events,
                               unsigned int flags) {
    unsigned int bytes;
    unsigned int copied = 0;
    process_t* proc;

    (void)regs;

    if ((flags & ~SYS_INPUT_FLAG_NONBLOCK) != 0u) return -EINVAL;
    if (max_events == 0u) return 0;
    if (max_events > INPUT_READ_MAX_EVENTS) return -EINVAL;
    if (!user_count_bytes_ok((unsigned int)out_events,
                             max_events,
                             sizeof(sys_input_event_t),
                             &bytes)) {
        return -EFAULT;
    }

    proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    while (1) {
        __asm__ volatile ("cli");
        if (input_available()) {
            break;
        }
        if (flags & SYS_INPUT_FLAG_NONBLOCK) {
            return 0;
        }
        proc->state = PROCESS_STATE_WAITING;
        input_set_waiting_process(proc);
        __asm__ volatile ("sti");
        __asm__ volatile ("hlt");
    }
    __asm__ volatile ("sti");

    while (copied < max_events) {
        sys_input_event_t ev;
        if (!input_pop_event(&ev)) {
            break;
        }
        k_memcpy(&out_events[copied], &ev, sizeof(ev));
        copied++;
    }

    return (int)copied;
}

static int sys_input_wait_until_impl(syscall_regs_t* regs,
                                     unsigned int deadline) {
    process_t* proc = (process_t*)sched_current();

    (void)regs;

    if (!proc) return -EINVAL;

    __asm__ volatile ("cli");
    if (input_available()) {
        __asm__ volatile ("sti");
        return 1;
    }
    if ((int)(timer_get_ticks() - deadline) >= 0) {
        __asm__ volatile ("sti");
        return 0;
    }

    proc->sleep_until = deadline;
    proc->state = PROCESS_STATE_SLEEPING;
    input_set_waiting_process(proc);
    sys_wait_until_current_running(proc);
    input_forget_waiting_process(proc);
    __asm__ volatile ("sti");

    return input_available() ? 1 : 0;
}

static int sys_sound_op_impl(unsigned int op, unsigned int arg1,
                             unsigned int arg2) {
    switch (op) {
    case SYS_SOUND_OP_PCM_U8:
    case SYS_SOUND_OP_PCM_U8_LEGACY:
    case SYS_SOUND_OP_PCM_U8_SB16_8: {
        sys_sound_pcm_u8_t req;
        unsigned int bytes;
        int rc = copy_from_user(&req, (const void*)arg1, sizeof(req));

        if (rc < 0) return rc;
        if (req.count == 0u || req.count > SYS_SOUND_PCM_MAX_SAMPLES ||
            req.sample_hz < SYS_SOUND_PCM_MIN_HZ ||
            req.sample_hz > SYS_SOUND_PCM_MAX_HZ) {
            return -EINVAL;
        }
        if (!user_count_bytes_ok((unsigned int)req.samples, req.count,
                                 sizeof(req.samples[0]), &bytes)) {
            return -EFAULT;
        }
        (void)bytes;
        if (op == SYS_SOUND_OP_PCM_U8_LEGACY) {
            return sound_pcm_u8_legacy(req.samples, req.count, req.sample_hz);
        }
        if (op == SYS_SOUND_OP_PCM_U8_SB16_8) {
            return sound_pcm_u8_sb16_8(req.samples, req.count, req.sample_hz);
        }
        return sound_pcm_u8(req.samples, req.count, req.sample_hz);
    }
    case SYS_SOUND_OP_PIT_SEQUENCE: {
        sys_sound_pit_sequence_t req;
        unsigned int bytes;
        int rc = copy_from_user(&req, (const void*)arg1, sizeof(req));

        if (rc < 0) return rc;
        if (req.count == 0u || req.count > SYS_SOUND_SEQUENCE_MAX_SAMPLES ||
            req.sample_hz == 0u || req.divisor_scale == 0u) {
            return -EINVAL;
        }
        if (!user_count_bytes_ok((unsigned int)req.samples, req.count,
                                 sizeof(req.samples[0]), &bytes)) {
            return -EFAULT;
        }
        (void)bytes;
        return sound_pit_sequence(req.samples, req.count, req.sample_hz,
                                  req.divisor_scale);
    }
    case SYS_SOUND_OP_OPL_SEQUENCE: {
        sys_sound_opl_sequence_t req;
        unsigned int bytes;
        int rc = copy_from_user(&req, (const void*)arg1, sizeof(req));

        if (rc < 0) return rc;
        if (req.count == 0u ||
            req.count > SYS_SOUND_OPL_SEQUENCE_MAX_EVENTS ||
            req.timer_hz == 0u ||
            (req.flags & ~SYS_SOUND_OPL_SEQUENCE_FLAG_LOOP) != 0u) {
            return -EINVAL;
        }
        if (!user_count_bytes_ok((unsigned int)req.events,
                                 req.count,
                                 sizeof(req.events[0]),
                                 &bytes)) {
            return -EFAULT;
        }
        (void)bytes;
        return sound_opl_sequence(req.events, req.count,
                                  req.timer_hz, req.flags);
    }
    case SYS_SOUND_OP_OPL_EFFECT: {
        sys_sound_opl_effect_t req;
        unsigned int bytes;
        int rc = copy_from_user(&req, (const void*)arg1, sizeof(req));

        if (rc < 0) return rc;
        if (req.count == 0u ||
            req.count > SYS_SOUND_OPL_EFFECT_MAX_SAMPLES ||
            req.sample_hz == 0u ||
            req.sample_hz > SYS_SOUND_MAX_HZ ||
            req.channel >= 9u) {
            return -EINVAL;
        }
        if (!user_count_bytes_ok((unsigned int)req.samples,
                                 req.count,
                                 sizeof(req.samples[0]),
                                 &bytes)) {
            return -EFAULT;
        }
        (void)bytes;
        return sound_opl_effect(&req);
    }
    case SYS_SOUND_OP_TONE:
        return sound_tone(arg1, arg2);
    case SYS_SOUND_OP_STOP:
        sound_stop();
        return 0;
    case SYS_SOUND_OP_PIT_DIVISOR:
        return sound_pit_divisor(arg1, arg2);
    case SYS_SOUND_OP_OPL_WRITE:
        return sound_opl_write(arg1, arg2);
    case SYS_SOUND_OP_OPL_RESET:
        return sound_opl_reset();
    case SYS_SOUND_OP_OPL_SEQUENCE_STOP:
        sound_opl_sequence_stop();
        return 0;
    case SYS_SOUND_OP_OPL_EFFECT_STOP:
        sound_opl_effect_stop();
        return 0;
    case SYS_SOUND_OP_CAPS:
        return (int)sound_caps();
    case SYS_SOUND_OP_STATUS: {
        sys_sound_status_t status;

        if (!user_buf_ok(arg1, sizeof(status))) {
            return -EFAULT;
        }
        sound_status(&status);
        return copy_to_user((void*)arg1, &status, sizeof(status));
    }
    default:
        return -EINVAL;
    }
}

static int sys_fsinfo_impl(sys_fsinfo_t* out_info) {
    ext2_fsinfo_t info;
    sys_fsinfo_t user_info;

    if (!out_info) return -EFAULT;
    if (!ext2_fsinfo(&info)) return -EIO;

    user_info.total_bytes = info.total_bytes;
    user_info.used_bytes = info.used_bytes;
    user_info.free_bytes = info.free_bytes;
    user_info.cluster_bytes = info.cluster_bytes;
    user_info.total_clusters = info.total_clusters;
    user_info.free_clusters = info.free_clusters;
    if (copy_to_user(out_info, &user_info, sizeof(user_info)) < 0) return -EFAULT;
    return 0;
}

static int sys_fsmap_impl(sys_fsmap_request_t* user_req) {
    sys_fsmap_request_t req;
    u32 out_clusters = 0;

    if (!user_req) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;

    if (req.max_clusters == 0) {
        req.out_clusters = 0;
        if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
        return 0;
    }
    if (!req.states) return -EFAULT;
    if (!user_buf_ok((unsigned int)req.states, req.max_clusters)) return -EFAULT;
    if (!ext2_fsmap(req.start_cluster, req.max_clusters, req.states, &out_clusters)) return -EIO;

    req.out_clusters = out_clusters;
    if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
    return 0;
}

static int sys_meminfo_impl(sys_meminfo_t* out_info) {
    sys_meminfo_t info;

    if (!out_info) return -EFAULT;

    info.heap_base = memory_get_heap_base();
    info.heap_top = memory_get_heap_top();
    info.pmm_free_frames = pmm_free_count();
    info.pmm_total_frames = pmm_total_count();
    info.e820_valid = boot_info_e820_valid() ? 1u : 0u;
    info.e820_count = info.e820_valid ? boot_info_e820_count() : 0u;
    vfs_file_map_cache_stats(&info.ro_file_cache_pages,
                             &info.ro_file_cache_mapped_refs);

    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static unsigned int process_ram_bytes(process_t* proc) {
    unsigned int frames = 0;

    if (!proc) return 0;

    frames += 1u; /* process_t */
    frames += proc->kernel_stack_frames;
    frames += proc->fd_table_frames;
    frames += process_pd_count_private_frames(proc->pd);

    return frames * PAGE_SIZE;
}

static int sys_procinfo_impl(sys_procinfo_t* out_info) {
    process_t* procs[SYS_PROCINFO_MAX];
    sys_procinfo_t info;
    int count;
    unsigned int total_ticks = timer_get_ticks();

    if (!out_info) return -EFAULT;

    k_memset(&info, 0, sizeof(info));
    count = sched_snapshot_all(procs, SYS_PROCINFO_MAX);
    info.total_count = (unsigned int)count;
    info.out_count = (unsigned int)count;
    info.total_ticks = total_ticks;

    for (int i = 0; i < count; i++) {
        process_t* proc = procs[i];
        sys_procinfo_entry_t* ent = &info.entries[i];
        unsigned int heap_bytes = 0;

        if (!proc) continue;
        if (proc->heap_brk >= proc->heap_base) {
            heap_bytes = proc->heap_brk - proc->heap_base;
        }

        ent->pid = proc->pid;
        ent->parent_pid = proc->parent_pid;
        ent->pgid = proc->pgid;
        ent->state = (unsigned int)proc->state;
        ent->cpu_ticks = proc->cpu_ticks;
        ent->ram_bytes = process_ram_bytes(proc);
        ent->heap_bytes = heap_bytes;
        k_strncpy(ent->name, proc->name, sizeof(ent->name));
    }

    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_e820_entry_impl(unsigned int index, sys_e820_entry_t* out_entry) {
    const boot_info_t* info;
    sys_e820_entry_t entry;

    if (!out_entry) return -EFAULT;
    if (!boot_info_e820_valid()) return 0;

    info = boot_info_get();
    if (index >= info->e820_count) return -EINVAL;

    entry.base = info->e820[index].base;
    entry.length = info->e820[index].length;
    entry.type = info->e820[index].type;
    entry.attr = info->e820[index].attr;
    if (copy_to_user(out_entry, &entry, sizeof(entry)) < 0) return -EFAULT;
    return (int)info->e820_count;
}

static int sys_netinfo_impl(sys_netinfo_t* out_info) {
    sys_netinfo_t info;
    const u8* mac;
    const net_ipv4_config_t* cfg;
    nic_stats_t nic_stats;
    socket_stats_t socket_stats;
    tcp_stats_t tcp_stats;

    if (!out_info) return -EFAULT;
    k_memset(&info, 0, sizeof(info));

    info.net_link_up = nic_link_up() ? 1u : 0u;
    k_strncpy(info.net_driver, nic_driver_name(), sizeof(info.net_driver));
    mac = nic_mac();
    if (mac) {
        for (unsigned int i = 0; i < 6u; i++) info.mac[i] = mac[i];
    }
    nic_get_stats(&nic_stats);
    info.nic_tx_packets = nic_stats.tx_packets;
    info.nic_rx_packets = nic_stats.rx_packets;
    info.nic_tx_errors = nic_stats.tx_errors;
    info.nic_rx_errors = nic_stats.rx_errors;
    info.nic_status = nic_stats.status;
    info.nic_command = nic_stats.command;
    info.nic_rx_config = nic_stats.rx_config;
    info.nic_tx_config = nic_stats.tx_config;
    info.nic_rx_cursor = nic_stats.rx_cursor;
    info.nic_rx_hw_cursor = nic_stats.rx_hw_cursor;

    cfg = net_ipv4_config();
    if (cfg) {
        info.ipv4_configured = cfg->configured ? 1u : 0u;
        info.ip = cfg->ip;
        info.netmask = cfg->netmask;
        info.gateway = cfg->gateway;
        info.dns = cfg->dns;
        info.dhcp_server = cfg->dhcp_server;
        info.lease_seconds = cfg->lease_seconds;
    }

    socket_get_stats(&socket_stats);
    info.max_sockets = socket_stats.max_sockets;
    info.used_sockets = socket_stats.used_sockets;
    info.tcp_sockets = socket_stats.tcp_sockets;
    info.open_sockets = socket_stats.open_sockets;
    info.bound_sockets = socket_stats.bound_sockets;
    info.listening_sockets = socket_stats.listening_sockets;
    info.connected_sockets = socket_stats.connected_sockets;

    tcp_get_stats(&tcp_stats);
    info.tcp_listeners = tcp_stats.listeners;
    info.tcp_max_listeners = tcp_stats.max_listeners;
    info.tcp_connections = tcp_stats.connections;
    info.tcp_max_connections = tcp_stats.max_connections;
    info.tcp_established_connections = tcp_stats.established_connections;
    info.tcp_accepted_connections = tcp_stats.accepted_connections;
    info.tcp_pending_connections = tcp_stats.pending_connections;
    info.tcp_syn_recv_connections = tcp_stats.syn_recv_connections;
    info.tcp_fin_wait_connections = tcp_stats.fin_wait_connections;
    info.tcp_rx_rings = tcp_stats.rx_rings;
    info.tcp_tx_rings = tcp_stats.tx_rings;
    info.tcp_rx_bytes = tcp_stats.rx_bytes;
    info.tcp_tx_bytes = tcp_stats.tx_bytes;
    info.tcp_rx_buffer_bytes = tcp_stats.rx_buffer_bytes;
    info.tcp_tx_buffer_bytes = tcp_stats.tx_buffer_bytes;
    info.tcp_max_rx_buffer_bytes = tcp_stats.max_rx_buffer_bytes;
    info.tcp_max_tx_buffer_bytes = tcp_stats.max_tx_buffer_bytes;

    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_net_route_for_target(u32 target_ip, u32* out_sender_ip, u32* out_next_hop) {
    u32 sender_ip = net_ipv4_local_ip();
    u32 netmask = net_ipv4_netmask();
    u32 gateway = net_ipv4_gateway();
    u32 next_hop = target_ip;

    if (!net_ipv4_is_configured() || sender_ip == 0u) return -ENETUNREACH;
    if (netmask != 0u && (target_ip & netmask) != (sender_ip & netmask)) {
        if (gateway == 0u) return -ENETUNREACH;
        next_hop = gateway;
    }

    if (out_sender_ip) *out_sender_ip = sender_ip;
    if (out_next_hop) *out_next_hop = next_hop;
    return 0;
}

static int sys_net_op_impl(sys_net_op_request_t* user_req) {
    sys_net_op_request_t req;
    int rc;

    if (!user_req) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;

    switch (req.op) {
    case SYS_NET_OP_SEND_TEST_FRAME:
        return nic_send_test_frame() ? 1 : -EIO;
    case SYS_NET_OP_POLL_ONCE:
        return net_poll_once() ? 1 : 0;
    case SYS_NET_OP_DHCP:
        return dhcp_configure() ? 1 : 0;
    case SYS_NET_OP_CONFIGURE:
        if (req.target_ip == 0u) return -EINVAL;
        net_ipv4_configure(req.target_ip, req.netmask, req.gateway, req.dns,
                           req.dhcp_server, req.lease_seconds);
        return 1;
    case SYS_NET_OP_CLEAR_CONFIG:
        net_ipv4_clear_config();
        return 1;
    case SYS_NET_OP_ARP:
        if (req.target_ip == 0u) req.target_ip = net_ipv4_gateway();
        rc = sys_net_route_for_target(req.target_ip, &req.sender_ip, &req.next_hop_ip);
        if (rc < 0) return rc;
        if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
        if (!arp_resolve(req.sender_ip, req.next_hop_ip, req.mac)) return 0;
        if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
        return 1;
    case SYS_NET_OP_PING:
        if (req.target_ip == 0u) req.target_ip = net_ipv4_gateway();
        rc = sys_net_route_for_target(req.target_ip, &req.sender_ip, &req.next_hop_ip);
        if (rc < 0) return rc;
        if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
        return ipv4_ping_via_gateway(req.sender_ip, req.target_ip, req.next_hop_ip) ? 1 : 0;
    default:
        return -EINVAL;
    }
}

static int sys_block_read_sector_impl(unsigned int lba, void* user_buf) {
    block_device_t* dev = ext2_block_device();
    int rc = 0;

    if (!user_buf) return -EFAULT;
    if (!user_buf_ok((unsigned int)user_buf, sizeof(s_sys_block_sector))) return -EFAULT;
    if (!dev || dev->sector_size != sizeof(s_sys_block_sector)) return -EIO;

    while (!__sync_bool_compare_and_swap(&s_sys_block_sector_locked, 0, 1)) {
        __asm__ __volatile__("" : : : "memory");
    }

    if (!block_read(dev, lba, 1, s_sys_block_sector)) {
        rc = -EIO;
        goto out;
    }
    if (copy_to_user(user_buf, s_sys_block_sector, sizeof(s_sys_block_sector)) < 0) {
        rc = -EFAULT;
        goto out;
    }

out:
    __sync_lock_release(&s_sys_block_sector_locked);
    return rc;
}

static int sys_getcwd_impl(char* buf, unsigned int size) {
    process_t* proc = (process_t*)sched_current();
    unsigned int pos = 0;

    if (!proc) return -EINVAL;
    if (!buf || size == 0) return -EFAULT;
    if (!user_buf_ok((unsigned int)buf, size)) return -EFAULT;

    if (size < 2) return -EINVAL;
    buf[pos++] = '/';
    for (unsigned int i = 0; proc->cwd[i] != '\0'; i++) {
        if (pos + 1 >= size) return -EINVAL;
        buf[pos++] = proc->cwd[i];
    }
    buf[pos] = '\0';
    return 0;
}

static int sys_chdir_impl(const char* path) {
    char kpath[PROCESS_CWD_MAX];
    process_t* proc = (process_t*)sched_current();

    if (!proc) return -EINVAL;
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    if (!virtual_path_is_dir(kpath) && !vfs_is_dir(kpath)) {
        u32 size = 0;
        int is_dir = 0;
        return virtual_path_exists(kpath, &size, &is_dir, 0) || vfs_stat(kpath, &size, &is_dir)
            ? -ENOTDIR
            : path_lookup_errno(kpath);
    }
    {
        int perm_rc = check_path_permission(proc, kpath, SYS_PERM_X, 0);
        if (perm_rc < 0) return perm_rc;
    }

    k_memcpy(proc->cwd, kpath, (k_size_t)k_strlen(kpath) + 1u);
    return 0;
}

static int sys_fread_impl(int fd, char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    return process_fd_read(ent, buf, len);
}

void syscall_handler_main(syscall_regs_t* regs) {
    if (regs == 0) return;

    switch (regs->eax) {
        case SYS_WRITE:
            regs->eax = (unsigned int)sys_write_impl(
                            (const char*)regs->ebx, regs->ecx);
            break;

        case SYS_EXIT:
            sys_exit_impl(regs);
            break;

        case SYS_GET_TICKS:
            regs->eax = sys_get_ticks_impl();
            break;

        case SYS_PUTC:
            regs->eax = (unsigned int)sys_putc_impl(regs->ebx);
            break;

        case SYS_READ:
            regs->eax = (unsigned int)sys_read_impl(
                            (char*)regs->ebx,
                            regs->ecx);
            break;

        case SYS_READ_RAW:
            regs->eax = (unsigned int)sys_read_raw_impl(
                            (char*)regs->ebx,
                            regs->ecx);
            break;

        case SYS_YIELD:
            regs->eax = (unsigned int)sys_yield_impl();
            break;

        case SYS_SLEEP:
            regs->eax = (unsigned int)sys_sleep_impl(regs, regs->ebx);
            break;

        case SYS_WRITEFILE:
            regs->eax = (unsigned int)sys_writefile_impl(
                            (const char*)regs->ebx,
                            (const void*)regs->ecx,
                            regs->edx);
            break;

        case SYS_WRITEFILE_PATH:
            regs->eax = (unsigned int)sys_writefile_path_impl(
                            (const char*)regs->ebx,
                            (const void*)regs->ecx,
                            regs->edx);
            break;

        case SYS_BRK:
            regs->eax = sys_brk_impl(regs->ebx);
            break;

        case SYS_MMAP:
            regs->eax = (unsigned int)sys_mmap_impl(regs->ebx,
                                                    regs->ecx,
                                                    regs->edx,
                                                    regs->esi,
                                                    (int)regs->edi,
                                                    regs->ebp);
            break;

        case SYS_MUNMAP:
            regs->eax = (unsigned int)sys_munmap_impl(regs->ebx,
                                                      regs->ecx);
            break;

        case SYS_MPROTECT:
            regs->eax = (unsigned int)sys_mprotect_impl(regs->ebx,
                                                        regs->ecx,
                                                        regs->edx);
            break;

        case SYS_HALT:
            system_halt();
            regs->eax = 0;
            break;

        case SYS_REBOOT:
            system_reboot();
            regs->eax = 0;
            break;

        case SYS_EXEC:
            regs->eax = (unsigned int)sys_exec_impl(
                            (const char*)regs->ebx,
                            (int)regs->ecx,
                            (char**)regs->edx);
            break;

        case SYS_EXEC_FG:
            regs->eax = (unsigned int)sys_exec_fg_impl(
                            (const char*)regs->ebx,
                            (int)regs->ecx,
                            (char**)regs->edx);
            break;

        case SYS_GETPID:
            regs->eax = (unsigned int)sys_getpid_impl();
            break;

        case SYS_WAITPID:
            regs->eax = (unsigned int)sys_waitpid_impl(
                            (int)regs->ebx,
                            (int*)regs->ecx,
                            (int)regs->edx);
            break;

        case SYS_WAITPID_FG:
            regs->eax = (unsigned int)sys_waitpid_fg_impl(
                            (int)regs->ebx,
                            (int*)regs->ecx);
            break;

        case SYS_KILL:
            regs->eax = (unsigned int)sys_kill_impl(
                            regs,
                            (int)regs->ebx,
                            (int)regs->ecx);
            break;

        case SYS_OPEN:
            regs->eax = (unsigned int)sys_open_impl(
                            (const char*)regs->ebx);
            break;

        case SYS_OPEN_WRITE:
            regs->eax = (unsigned int)sys_open_write_impl(
                            (const char*)regs->ebx);
            break;

        case SYS_OPEN_MODE:
            regs->eax = (unsigned int)sys_open_mode_impl(
                            (const char*)regs->ebx,
                            (unsigned int)regs->ecx);
            break;

        case SYS_OPEN_CREATE_MODE:
            regs->eax = (unsigned int)sys_open_mode_create_impl(
                            (const char*)regs->ebx,
                            (unsigned int)regs->ecx,
                            (unsigned int)regs->edx);
            break;

        case SYS_OPENAT_CREATE_MODE:
            regs->eax = (unsigned int)sys_openat_mode_create_impl(
                            (int)regs->ebx,
                            (const char*)regs->ecx,
                            (unsigned int)regs->edx,
                            (unsigned int)regs->esi);
            break;

        case SYS_CLOSE:
        {
            int fd = (int)regs->ebx;
            process_t* proc = (process_t*)sched_current();
            fd_entry_t* ent = process_fd_get(proc, fd);
            if (ent && ent->writable) {
                if (!process_fd_flush(ent)) {
                    regs->eax = (unsigned int)-EIO;
                    break;
                }
            }
            regs->eax = (unsigned int)sys_close_impl(fd);
            break;
        }

        case SYS_FREAD:
            regs->eax = (unsigned int)sys_fread_impl(
                            (int)regs->ebx,
                            (char*)regs->ecx,
                            regs->edx);
            break;

        case SYS_WRITEFD:
            regs->eax = (unsigned int)sys_writefd_impl(
                            (int)regs->ebx,
                            (const char*)regs->ecx,
                            regs->edx);
            break;

        case SYS_LSEEK:
            regs->eax = (unsigned int)sys_lseek_impl(
                            (int)regs->ebx,
                            (int)regs->ecx,
                            (int)regs->edx);
            break;

        case SYS_UNLINK:
            regs->eax = (unsigned int)sys_unlink_impl((const char*)regs->ebx);
            break;

        case SYS_RENAME:
            regs->eax = (unsigned int)sys_rename_impl((const char*)regs->ebx,
                                                      (const char*)regs->ecx);
            break;

        case SYS_STAT:
            regs->eax = (unsigned int)sys_stat_impl((const char*)regs->ebx,
                                                    (unsigned int*)regs->ecx,
                                                    (int*)regs->edx);
            break;

        case SYS_SOCKET:
            regs->eax = (unsigned int)sys_socket_impl((int)regs->ebx,
                                                      (int)regs->ecx,
                                                      (int)regs->edx);
            break;

        case SYS_BIND:
            regs->eax = (unsigned int)sys_bind_impl((int)regs->ebx,
                                                    (const struct sockaddr*)regs->ecx,
                                                    regs->edx);
            break;

        case SYS_LISTEN:
            regs->eax = (unsigned int)sys_listen_impl((int)regs->ebx,
                                                      (int)regs->ecx);
            break;

        case SYS_ACCEPT:
        {
            regs->eax = (unsigned int)sys_accept_impl(regs,
                                                      (int)regs->ebx,
                                                      (struct sockaddr*)regs->ecx,
                                                      (unsigned int*)regs->edx,
                                                      0u);
            break;
        }

        case SYS_CONNECT:
            regs->eax = (unsigned int)sys_connect_impl((int)regs->ebx,
                                                       (const struct sockaddr*)regs->ecx,
                                                       regs->edx);
            break;

        case SYS_SEND:
            regs->eax = (unsigned int)sys_send_impl((int)regs->ebx,
                                                    (const void*)regs->ecx,
                                                    regs->edx);
            break;

        case SYS_RECV:
            regs->eax = (unsigned int)sys_recv_impl(regs,
                                                    (int)regs->ebx,
                                                    (void*)regs->ecx,
                                                    regs->edx);
            break;

        case SYS_SENDTO:
            regs->eax = (unsigned int)sys_sendto_impl((int)regs->ebx,
                                                      (const void*)regs->ecx,
                                                      regs->edx,
                                                      regs->esi,
                                                      (const struct sockaddr*)regs->edi,
                                                      regs->ebp);
            break;

        case SYS_RECVFROM:
            regs->eax = (unsigned int)sys_recvfrom_impl(regs,
                                                        (int)regs->ebx,
                                                        (void*)regs->ecx,
                                                        regs->edx,
                                                        regs->esi,
                                                        (struct sockaddr*)regs->edi,
                                                        (unsigned int*)regs->ebp);
            break;

        case SYS_POLL:
            regs->eax = (unsigned int)sys_poll_impl(regs,
                                                    (struct pollfd*)regs->ebx,
                                                    regs->ecx,
                                                    (int)regs->edx);
            break;

        case SYS_MKDIR:
            regs->eax = (unsigned int)sys_mkdir_impl((const char*)regs->ebx,
                                                     regs->ecx);
            break;

        case SYS_MKDIRAT:
            regs->eax = (unsigned int)sys_mkdirat_impl((int)regs->ebx,
                                                       (const char*)regs->ecx,
                                                       regs->edx);
            break;

        case SYS_RMDIR:
            regs->eax = (unsigned int)sys_rmdir_impl((const char*)regs->ebx);
            break;

        case SYS_DIRLIST:
            regs->eax = (unsigned int)sys_dirlist_impl((const char*)regs->ebx,
                                                       regs->ecx,
                                                       (uapi_dirent_t*)regs->edx);
            break;

        case SYS_DIRLIST_BATCH:
            regs->eax = (unsigned int)sys_dirlist_batch_impl((const char*)regs->ebx,
                                                             regs->ecx,
                                                             (uapi_dirent_t*)regs->edx,
                                                             regs->esi);
            break;

        case SYS_SETSOCKOPT:
            regs->eax = (unsigned int)sys_setsockopt_impl((int)regs->ebx,
                                                          (int)regs->ecx,
                                                          (int)regs->edx);
            break;

        case SYS_GETSOCKNAME:
            regs->eax = (unsigned int)sys_getsockname_impl((int)regs->ebx,
                                                           (struct sockaddr*)regs->ecx,
                                                           (unsigned int*)regs->edx);
            break;

        case SYS_GETCWD:
            regs->eax = (unsigned int)sys_getcwd_impl((char*)regs->ebx,
                                                      regs->ecx);
            break;

        case SYS_CHDIR:
            regs->eax = (unsigned int)sys_chdir_impl((const char*)regs->ebx);
            break;

        case SYS_FSYNC:
            regs->eax = (unsigned int)sys_fsync_impl((int)regs->ebx);
            break;

        case SYS_FCNTL:
            regs->eax = (unsigned int)sys_fcntl_impl((int)regs->ebx,
                                                     (int)regs->ecx,
                                                     regs->edx);
            break;

        case SYS_PIPE:
            regs->eax = (unsigned int)sys_pipe2_impl((int*)regs->ebx, 0);
            break;

        case SYS_PIPE2:
            regs->eax = (unsigned int)sys_pipe2_impl((int*)regs->ebx, regs->ecx);
            break;

        case SYS_PTY_OPEN:
            regs->eax = (unsigned int)sys_pty_open_impl((int*)regs->ebx, regs->ecx);
            break;

        case SYS_PTY_SET_SIZE:
            regs->eax = (unsigned int)sys_pty_set_size_impl((int)regs->ebx,
                                                            regs->ecx,
                                                            regs->edx);
            break;

        case SYS_TCGETATTR:
            regs->eax = (unsigned int)sys_tcgetattr_impl((int)regs->ebx,
                                                         (sys_termios_t*)regs->ecx);
            break;

        case SYS_TCSETATTR:
            regs->eax = (unsigned int)sys_tcsetattr_impl((int)regs->ebx,
                                                         (const sys_termios_t*)regs->ecx);
            break;

        case SYS_TTY_IOCTL:
            regs->eax = (unsigned int)sys_tty_ioctl_impl((int)regs->ebx,
                                                         regs->ecx,
                                                         (void*)regs->edx);
            break;

        case SYS_GETRLIMIT:
            regs->eax = (unsigned int)sys_getrlimit_impl((int)regs->ebx,
                                                         (sys_rlimit_t*)regs->ecx);
            break;

        case SYS_SETRLIMIT:
            regs->eax = (unsigned int)sys_setrlimit_impl((int)regs->ebx,
                                                         (const sys_rlimit_t*)regs->ecx);
            break;

        case SYS_GETRUSAGE:
            regs->eax = (unsigned int)sys_getrusage_impl((int)regs->ebx,
                                                         (sys_rusage_t*)regs->ecx);
            break;

        case SYS_SETSID:
            regs->eax = (unsigned int)sys_setsid_impl();
            break;

        case SYS_GETSID:
            regs->eax = (unsigned int)sys_getsid_impl((int)regs->ebx);
            break;

        case SYS_SETPGID:
            regs->eax = (unsigned int)sys_setpgid_impl((int)regs->ebx,
                                                       (int)regs->ecx);
            break;

        case SYS_GETPGID:
            regs->eax = (unsigned int)sys_getpgid_impl((int)regs->ebx);
            break;

        case SYS_MOUNT:
            regs->eax = (unsigned int)sys_mount_impl((const char*)regs->ebx,
                                                     (const char*)regs->ecx,
                                                     (const char*)regs->edx,
                                                     regs->esi,
                                                     (const void*)regs->edi);
            break;

        case SYS_UMOUNT2:
            regs->eax = (unsigned int)sys_umount2_impl((const char*)regs->ebx,
                                                       regs->ecx);
            break;

        case SYS_STATFS:
            regs->eax = (unsigned int)sys_statfs_impl((const char*)regs->ebx,
                                                      (sys_statfs_t*)regs->ecx);
            break;

        case SYS_FSTATFS:
            regs->eax = (unsigned int)sys_fstatfs_impl((int)regs->ebx,
                                                       (sys_statfs_t*)regs->ecx);
            break;

        case SYS_STAT_FULL:
            regs->eax = (unsigned int)sys_stat_full_impl((const char*)regs->ebx,
                                                         (sys_stat_info_t*)regs->ecx);
            break;

        case SYS_FSTAT_FULL:
            regs->eax = (unsigned int)sys_fstat_full_impl((int)regs->ebx,
                                                          (sys_stat_info_t*)regs->ecx);
            break;

        case SYS_DUP:
        {
            process_t* proc = (process_t*)sched_current();
            regs->eax = (unsigned int)process_fd_dup(proc, (int)regs->ebx, 0, 0);
            break;
        }

        case SYS_DUP2:
        {
            process_t* proc = (process_t*)sched_current();
            regs->eax = (unsigned int)process_fd_dup2(proc,
                                                      (int)regs->ebx,
                                                      (int)regs->ecx,
                                                      0,
                                                      0);
            break;
        }

        case SYS_DUP3:
        {
            process_t* proc = (process_t*)sched_current();
            unsigned int fd_flags = 0;
            if ((regs->edx & ~SYS_FD_FLAG_CLOEXEC) != 0u) {
                regs->eax = (unsigned int)-EINVAL;
            } else {
                if (regs->edx & SYS_FD_FLAG_CLOEXEC) fd_flags = SYS_FD_FLAG_CLOEXEC;
                regs->eax = (unsigned int)process_fd_dup2(proc,
                                                          (int)regs->ebx,
                                                          (int)regs->ecx,
                                                          fd_flags,
                                                          1);
            }
            break;
        }

        case SYS_FORK:
            regs->eax = (unsigned int)sys_fork_impl(regs);
            break;

        case SYS_EXECVE:
            regs->eax = (unsigned int)sys_execve_impl(regs,
                                                      (const char*)regs->ebx,
                                                      (char**)regs->ecx,
                                                      (char**)regs->edx);
            break;

        case SYS_EPOLL_CREATE:
            regs->eax = (unsigned int)sys_epoll_create_impl((int)regs->ebx);
            break;

        case SYS_EPOLL_CTL:
            regs->eax = (unsigned int)sys_epoll_ctl_impl((int)regs->ebx,
                                                         (int)regs->ecx,
                                                         (int)regs->edx,
                                                         (struct epoll_event*)regs->esi);
            break;

        case SYS_EPOLL_WAIT:
            regs->eax = (unsigned int)sys_epoll_wait_impl(regs,
                                                          (int)regs->ebx,
                                                          (struct epoll_event*)regs->ecx,
                                                          (int)regs->edx,
                                                          (int)regs->esi);
            break;

        case SYS_TIMERFD_CREATE:
            regs->eax = (unsigned int)sys_timerfd_create_impl((int)regs->ebx,
                                                              (int)regs->ecx);
            break;

        case SYS_TIMERFD_SETTIME:
            regs->eax = (unsigned int)sys_timerfd_settime_impl(
                            (int)regs->ebx,
                            (int)regs->ecx,
                            (const struct user_itimerspec*)regs->edx,
                            (struct user_itimerspec*)regs->esi);
            break;

        case SYS_SIGNALFD:
            regs->eax = (unsigned int)sys_signalfd_impl((int)regs->ebx,
                                                        (const void*)regs->ecx,
                                                        (int)regs->edx);
            break;

        case SYS_ACCEPT4:
            regs->eax = (unsigned int)sys_accept_impl(regs,
                                                      (int)regs->ebx,
                                                      (struct sockaddr*)regs->ecx,
                                                      (unsigned int*)regs->edx,
                                                      regs->esi);
            break;

        case SYS_SHUTDOWN:
            regs->eax = (unsigned int)sys_shutdown_impl((int)regs->ebx,
                                                       (int)regs->ecx);
            break;

        case SYS_GETPEERNAME:
            regs->eax = (unsigned int)sys_getpeername_impl((int)regs->ebx,
                                                           (struct sockaddr*)regs->ecx,
                                                           (unsigned int*)regs->edx);
            break;

        case SYS_FSTAT:
            regs->eax = (unsigned int)sys_fstat_impl((int)regs->ebx,
                                                     (unsigned int*)regs->ecx,
                                                     (int*)regs->edx);
            break;

        case SYS_TERMINAL_SIZE:
            regs->eax = (unsigned int)sys_terminal_size_impl(
                            (unsigned int*)regs->ebx,
                            (unsigned int*)regs->ecx);
            break;

        case SYS_DISPLAY_INFO:
            regs->eax = (unsigned int)sys_display_info_impl(
                            (sys_display_info_t*)regs->ebx);
            break;

        case SYS_DISPLAY_FILL:
            regs->eax = (unsigned int)sys_display_fill_impl(
                            (const sys_display_fill_rect_t*)regs->ebx);
            break;

        case SYS_DISPLAY_BLIT:
            regs->eax = (unsigned int)sys_display_blit_impl(
                            (const sys_display_blit_rect_t*)regs->ebx);
            break;

        case SYS_DISPLAY_BLIT_STRIDE:
            regs->eax = (unsigned int)sys_display_blit_stride_impl(
                            (const sys_display_blit_stride_rect_t*)regs->ebx);
            break;

        case SYS_DISPLAY_MAP:
            regs->eax = (unsigned int)sys_display_map_impl(
                            (sys_display_map_info_t*)regs->ebx);
            break;

        case SYS_DISPLAY_PRESENT_PAGE:
            regs->eax = (unsigned int)sys_display_present_page_impl(
                            (sys_display_present_page_t*)regs->ebx);
            break;

        case SYS_DISPLAY_ACQUIRE:
            regs->eax = (unsigned int)sys_display_acquire_impl();
            break;

        case SYS_DISPLAY_RELEASE:
            regs->eax = (unsigned int)sys_display_release_impl();
            break;

        case SYS_MOUSE_READ:
            regs->eax = (unsigned int)sys_mouse_read_impl(
                            (sys_mouse_state_t*)regs->ebx);
            break;

        case SYS_USB_MOUSE_OP:
            regs->eax = (unsigned int)sys_usb_mouse_op_impl(
                            regs->ebx,
                            regs->ecx);
            break;

        case SYS_USBINFO:
            regs->eax = (unsigned int)sys_usbinfo_impl(
                            (sys_usbinfo_t*)regs->ebx);
            break;

        case SYS_MOUSE_DEBUG:
            regs->eax = (unsigned int)sys_mouse_debug_impl(
                            (sys_mousedebug_t*)regs->ebx);
            break;

        case SYS_USB_DIAG_OP:
            regs->eax = (unsigned int)sys_usb_diag_op_impl(
                            regs->ebx,
                            regs->ecx);
            break;

        case SYS_INPUT_READ:
            regs->eax = (unsigned int)sys_input_read_impl(
                            regs,
                            (sys_input_event_t*)regs->ebx,
                            regs->ecx,
                            regs->edx);
            break;

        case SYS_INPUT_WAIT_UNTIL:
            regs->eax = (unsigned int)sys_input_wait_until_impl(
                            regs,
                            regs->ebx);
            break;

        case SYS_SOUND_OP:
            regs->eax = (unsigned int)sys_sound_op_impl(
                            regs->ebx,
                            regs->ecx,
                            regs->edx);
            break;

        case SYS_FSINFO:
            regs->eax = (unsigned int)sys_fsinfo_impl(
                            (sys_fsinfo_t*)regs->ebx);
            break;

        case SYS_FSMAP:
            regs->eax = (unsigned int)sys_fsmap_impl(
                            (sys_fsmap_request_t*)regs->ebx);
            break;

        case SYS_MEMINFO:
            regs->eax = (unsigned int)sys_meminfo_impl(
                            (sys_meminfo_t*)regs->ebx);
            break;

        case SYS_PROCINFO:
            regs->eax = (unsigned int)sys_procinfo_impl(
                            (sys_procinfo_t*)regs->ebx);
            break;

        case SYS_E820_ENTRY:
            regs->eax = (unsigned int)sys_e820_entry_impl(
                            regs->ebx,
                            (sys_e820_entry_t*)regs->ecx);
            break;

        case SYS_NETINFO:
            regs->eax = (unsigned int)sys_netinfo_impl(
                            (sys_netinfo_t*)regs->ebx);
            break;

        case SYS_NET_OP:
            regs->eax = (unsigned int)sys_net_op_impl(
                            (sys_net_op_request_t*)regs->ebx);
            break;

        case SYS_NET_IOCTL:
            regs->eax = (unsigned int)sys_net_ioctl_impl(
                            (int)regs->ebx,
                            regs->ecx,
                            (void*)regs->edx);
            break;

        case SYS_BLOCK_READ_SECTOR:
            regs->eax = (unsigned int)sys_block_read_sector_impl(
                            regs->ebx,
                            (void*)regs->ecx);
            break;

        case SYS_CLOCK_GETTIME:
            regs->eax = (unsigned int)sys_clock_gettime_impl(
                            (int)regs->ebx,
                            (struct user_timespec*)regs->ecx);
            break;

        case SYS_CLOCK_SETTIME:
            regs->eax = (unsigned int)sys_clock_settime_impl(
                            (int)regs->ebx,
                            (const struct user_timespec*)regs->ecx);
            break;

        case SYS_NTP_SYNC:
            regs->eax = (unsigned int)sys_ntp_sync_impl(
                            regs->ebx,
                            (struct user_timespec*)regs->ecx);
            break;

        case SYS_LINK:
            regs->eax = (unsigned int)sys_link_impl((const char*)regs->ebx,
                                                    (const char*)regs->ecx);
            break;

        case SYS_LINKAT:
            regs->eax = (unsigned int)sys_linkat_impl((int)regs->ebx,
                                                      (const char*)regs->ecx,
                                                      (int)regs->edx,
                                                      (const char*)regs->esi,
                                                      regs->edi);
            break;

        case SYS_SYMLINK:
            regs->eax = (unsigned int)sys_symlink_impl((const char*)regs->ebx,
                                                       (const char*)regs->ecx);
            break;

        case SYS_SYMLINKAT:
            regs->eax = (unsigned int)sys_symlinkat_impl((const char*)regs->ebx,
                                                         (int)regs->ecx,
                                                         (const char*)regs->edx);
            break;

        case SYS_READLINK:
            regs->eax = (unsigned int)sys_readlink_impl((const char*)regs->ebx,
                                                        (char*)regs->ecx,
                                                        regs->edx);
            break;

        case SYS_READLINKAT:
            regs->eax = (unsigned int)sys_readlinkat_impl((int)regs->ebx,
                                                          (const char*)regs->ecx,
                                                          (char*)regs->edx,
                                                          regs->esi);
            break;

        case SYS_LSTAT_FULL:
            regs->eax = (unsigned int)sys_lstat_full_impl(
                            (const char*)regs->ebx,
                            (sys_stat_info_t*)regs->ecx);
            break;

        case SYS_FSTATAT_FULL:
            regs->eax = (unsigned int)sys_fstatat_full_impl(
                            (int)regs->ebx,
                            (const char*)regs->ecx,
                            (sys_stat_info_t*)regs->edx,
                            regs->esi);
            break;

        case SYS_CHMOD:
            regs->eax = (unsigned int)sys_chmod_impl((const char*)regs->ebx,
                                                     regs->ecx);
            break;

        case SYS_CHOWN:
            regs->eax = (unsigned int)sys_chown_impl((const char*)regs->ebx,
                                                     regs->ecx,
                                                     regs->edx);
            break;

        case SYS_UTIMENS:
            regs->eax = (unsigned int)sys_utimens_impl(
                            (const char*)regs->ebx,
                            (const struct user_timespec*)regs->ecx);
            break;

        case SYS_UTIMENSAT:
            regs->eax = (unsigned int)sys_utimensat_impl(
                            (int)regs->ebx,
                            (const char*)regs->ecx,
                            (const struct user_timespec*)regs->edx,
                            regs->esi);
            break;

        case SYS_MKNOD:
            regs->eax = (unsigned int)sys_mknod_impl((const char*)regs->ebx,
                                                     regs->ecx,
                                                     regs->edx);
            break;

        case SYS_FTRUNCATE:
            regs->eax = (unsigned int)sys_ftruncate_impl((int)regs->ebx,
                                                         regs->ecx);
            break;

        case SYS_FCHMOD:
            regs->eax = (unsigned int)sys_fchmod_impl((int)regs->ebx,
                                                      regs->ecx);
            break;

        case SYS_FCHOWN:
            regs->eax = (unsigned int)sys_fchown_impl((int)regs->ebx,
                                                      regs->ecx,
                                                      regs->edx);
            break;

        case SYS_FUTIMENS:
            regs->eax = (unsigned int)sys_futimens_impl(
                            (int)regs->ebx,
                            (const struct user_timespec*)regs->ecx);
            break;

        case SYS_GETUID:
            regs->eax = sys_getuid_impl();
            break;

        case SYS_GETEUID:
            regs->eax = sys_geteuid_impl();
            break;

        case SYS_GETGID:
            regs->eax = sys_getgid_impl();
            break;

        case SYS_GETEGID:
            regs->eax = sys_getegid_impl();
            break;

        case SYS_SETUID:
            regs->eax = (unsigned int)sys_setuid_impl(regs->ebx);
            break;

        case SYS_SETGID:
            regs->eax = (unsigned int)sys_setgid_impl(regs->ebx);
            break;

        case SYS_UMASK:
            regs->eax = sys_umask_impl(regs->ebx);
            break;

        case SYS_UNLINKAT:
            regs->eax = (unsigned int)sys_unlinkat_impl((int)regs->ebx,
                                                        (const char*)regs->ecx,
                                                        regs->edx);
            break;

        case SYS_RENAMEAT:
            regs->eax = (unsigned int)sys_renameat_impl((int)regs->ebx,
                                                        (const char*)regs->ecx,
                                                        (int)regs->edx,
                                                        (const char*)regs->esi);
            break;

        default:
            regs->eax = (unsigned int)-ENOSYS;
            break;
    }
}
