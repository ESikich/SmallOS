#include "syscall_internal.h"
#include "idt.h"
#include "kalloc.h"
#include "klib.h"
#include "memory.h"
#include "paging.h"
#include "pmm.h"
#include "process.h"
#include "scheduler.h"
#include "socket.h"
#include "timer.h"
#include "uapi_errno.h"
#include "uapi_net.h"
#include "vfs.h"
#include "../drivers/arp.h"
#include "../drivers/ext2.h"
#include "../drivers/net.h"
#include "../drivers/nic.h"

#define DIRLIST_BATCH_MAX     64u
#define SYS_AT_SYMLINK_NOFOLLOW 0x100u
#define SYS_AT_REMOVEDIR      0x200u
#define SYS_AT_SYMLINK_FOLLOW 0x400u
#define SYS_MOUNT_MS_RDONLY      1u
#define SYS_MOUNT_MS_NOSUID      2u
#define SYS_MOUNT_MS_NODEV       4u
#define SYS_MOUNT_MS_NOEXEC      8u
#define SYS_MOUNT_MS_SYNCHRONOUS 16u
#define SYS_MOUNT_MS_REMOUNT     32u
#define SYS_MOUNT_MS_MANDLOCK    64u
#define SYS_MOUNT_MS_DIRSYNC     128u
#define SYS_MOUNT_MS_NOSYMFOLLOW 256u
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
#define SYS_MOUNT_STATIC_COUNT 3u
#define SYS_MODE_IFMT         0170000u
#define SYS_MODE_IFDIR        0040000u

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

static int sys_utimens_kpath_impl(const char* kpath,
                                  const struct user_timespec* times,
                                  int nofollow);

static unsigned char s_sys_block_sector[512] __attribute__((aligned(16)));
static volatile int s_sys_block_sector_locked = 0;

static kernel_mount_t s_mounts[SYS_MOUNT_MAX] = {
    { 1u, 0u, "rootfs", "",     "ext2",     "rw", 0u, SYS_STATFS_EXT2_MAGIC, 0u },
    { 1u, 0u, "proc",   "proc", "proc",     "rw", 0u, SYS_STATFS_PROC_MAGIC, 1u },
    { 1u, 0u, "dev",    "dev",  "devtmpfs", "rw", 0u, SYS_STATFS_DEV_MAGIC,  1u },
};

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

static void vbuf_put_hex2(char* out, unsigned int cap, unsigned int* pos, unsigned int value) {
    static const char hexdigits[] = "0123456789abcdef";
    vbuf_putc(out, cap, pos, hexdigits[(value >> 4) & 0xFu]);
    vbuf_putc(out, cap, pos, hexdigits[value & 0xFu]);
}

static void vbuf_put_ipv4(char* out, unsigned int cap, unsigned int* pos, unsigned int ip) {
    vbuf_put_uint(out, cap, pos, (ip >> 24) & 0xFFu);
    vbuf_putc(out, cap, pos, '.');
    vbuf_put_uint(out, cap, pos, (ip >> 16) & 0xFFu);
    vbuf_putc(out, cap, pos, '.');
    vbuf_put_uint(out, cap, pos, (ip >> 8) & 0xFFu);
    vbuf_putc(out, cap, pos, '.');
    vbuf_put_uint(out, cap, pos, ip & 0xFFu);
}

static void vbuf_put_mac(char* out,
                         unsigned int cap,
                         unsigned int* pos,
                         const unsigned char* mac) {
    for (unsigned int i = 0; i < ETH_ALEN; i++) {
        if (i) vbuf_putc(out, cap, pos, ':');
        vbuf_put_hex2(out, cap, pos, mac ? mac[i] : 0u);
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
        path_eq(path, "dev") || path_eq(path, "dev/fd") ||
        path_eq(path, "dev/pts")) {
        return 1;
    }
    if (virtual_proc_pid_path(path, &proc, &leaf)) {
        return proc && leaf && leaf[0] == '\0';
    }
    return 0;
}

static int virtual_path_is_dev_pts(const char* path) {
    const char* p;
    char translated[PROCESS_FD_NAME_MAX];

    path = virtual_effective_path(path, translated, sizeof(translated));
    if (!k_starts_with(path, "dev/pts/")) return 0;
    p = path + 8;
    if (*p == '\0') return 0;
    while (*p) {
        if (*p < '0' || *p > '9') return 0;
        p++;
    }
    return 1;
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
    if (path_eq(path, "dev/urandom")) {
        if (out_type) *out_type = PROCESS_VIRTUAL_URANDOM;
        return 1;
    }
    if (path_eq(path, "dev/tty") || path_eq(path, "dev/console") ||
        path_eq(path, "dev/fd/0") || path_eq(path, "dev/fd/1") ||
        path_eq(path, "dev/fd/2") || virtual_path_is_dev_pts(path)) {
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
        process_accounting_t proc_acct;
        kalloc_stats_t ka;
        process_accounting_snapshot(&proc_acct);
        (void)kalloc_stats(&ka);
        vbuf_put_kb_line(out, cap, &pos, "MemTotal", total);
        vbuf_put_kb_line(out, cap, &pos, "MemFree", free);
        vbuf_put_kb_line(out, cap, &pos, "MemAvailable", free);
        vbuf_put_kb_line(out, cap, &pos, "KernelHeap", memory_get_heap_top() - memory_get_heap_base());
        vbuf_put_kb_line(out, cap, &pos, "KallocUsed", ka.used_bytes);
        vbuf_put_kb_line(out, cap, &pos, "KallocFree", ka.free_bytes);
        vbuf_put_kb_line(out, cap, &pos, "ProcessPages", proc_acct.process_pages * PAGE_SIZE);
        vbuf_put_kb_line(out, cap, &pos, "KernelStackPages", proc_acct.kernel_stack_pages * PAGE_SIZE);
        vbuf_put_kb_line(out, cap, &pos, "FdTablePages", proc_acct.fd_table_pages * PAGE_SIZE);
        vbuf_put_kb_line(out, cap, &pos, "VmAreaPages", proc_acct.vm_area_pages * PAGE_SIZE);
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
    if (path_eq(path, "proc/lastfault")) {
        return idt_lastfault_render(out, cap);
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
    if (path_eq(path, "proc/net/arp")) {
        unsigned int sender_ip = 0u;
        unsigned int target_ip = 0u;
        unsigned char mac[ETH_ALEN];

        vbuf_puts(out, cap, &pos, "IP address       HW type     Flags       HW address            Mask     Device\n");
        if (arp_cache_get(&sender_ip, &target_ip, mac) &&
            sender_ip != 0u && target_ip != 0u) {
            vbuf_put_ipv4(out, cap, &pos, target_ip);
            vbuf_puts(out, cap, &pos, "     0x1         0x2         ");
            vbuf_put_mac(out, cap, &pos, mac);
            vbuf_puts(out, cap, &pos, "     *        eth0\n");
        }
        return pos;
    }
    if (path_eq(path, "proc/net/tcp") || path_eq(path, "proc/net/udp") ||
        path_eq(path, "proc/net/raw")) {
        vbuf_puts(out, cap, &pos, "  sl  local_address rem_address   st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout inode\n");
        return pos;
    }
    if (path_eq(path, "proc/net/unix")) {
        vbuf_puts(out, cap, &pos, "Num       RefCount Protocol Flags    Type St Inode Path\n");
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
        type == PROCESS_VIRTUAL_TTY ||
        type == PROCESS_VIRTUAL_URANDOM) {
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

int process_is_root(process_t* proc) {
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

int check_path_permission(process_t* proc,
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


int sys_writefile_impl(const char* name, const void* buf, unsigned int len) {
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
int sys_writefile_path_impl(const char* path, const void* buf, unsigned int len) {
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

int sys_open_impl(const char* name) {
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
int sys_close_impl(int fd) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (ent->writable && !process_fd_flush(ent)) return -EIO;

    process_fd_close(ent);
    return 0;
}

int sys_open_write_impl(const char* name) {
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

int sys_open_mode_create_impl(const char* name,
                                     unsigned int mode,
                                     unsigned int create_mode) {
    char kname[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kname, sizeof(kname), name);
    if (path_rc < 0) return path_rc;
    return sys_open_mode_create_kpath_impl(kname, mode, create_mode);
}

int sys_openat_mode_create_impl(int dirfd,
                                       const char* name,
                                       unsigned int mode,
                                       unsigned int create_mode) {
    char kname[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_at_resolved(kname, sizeof(kname), dirfd, name);
    if (path_rc < 0) return path_rc;
    return sys_open_mode_create_kpath_impl(kname, mode, create_mode);
}

int sys_open_mode_impl(const char* name, unsigned int mode) {
    return sys_open_mode_create_impl(name, mode, 0666u);
}


int sys_mkdir_impl(const char* path, unsigned int mode) {
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

int sys_mkdirat_impl(int dirfd, const char* path, unsigned int mode) {
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

int sys_rmdir_impl(const char* path) {
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
        "meminfo", "uptime", "stat", "lastfault", "mounts", "filesystems", "net", "self"
    };
    static const char* const proc_net_entries[] = {
        "arp", "dev", "raw", "route", "tcp", "udp", "unix"
    };
    static const char* const pid_entries[] = {
        "stat", "status", "cmdline", "comm"
    };
    static const char* const dev_entries[] = {
        "null", "zero", "urandom", "tty", "console", "fd", "pts"
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
            process_t* procs[SCHED_MAX_PROCS];
            int count = sched_snapshot_all(procs, SCHED_MAX_PROCS);
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
        is_dir = path_eq(name, "fd") || path_eq(name, "pts");
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

int sys_dirlist_impl(const char* path, unsigned int index, uapi_dirent_t* out) {
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

int sys_dirlist_batch_impl(const char* path,
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


int sys_writefd_impl(int fd, const char* buf, unsigned int len) {
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

int sys_lseek_impl(int fd, int offset, int whence) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    return process_fd_seek(ent, offset, whence);
}

int sys_fsync_impl(int fd) {
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;

    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (!ent->writable) return 0;
    return process_fd_flush(ent) ? 0 : -EIO;
}

int sys_unlink_impl(const char* path) {
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

int sys_unlinkat_impl(int dirfd, const char* path, unsigned int flags) {
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

int sys_link_impl(const char* oldpath, const char* newpath) {
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

int sys_linkat_impl(int olddirfd,
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

int sys_symlink_impl(const char* target, const char* linkpath) {
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

int sys_symlinkat_impl(const char* target, int newdirfd, const char* linkpath) {
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

int sys_readlink_impl(const char* path, char* out, unsigned int out_size) {
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

int sys_readlinkat_impl(int dirfd, const char* path, char* out, unsigned int out_size) {
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

int sys_rename_impl(const char* src, const char* dst) {
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

int sys_renameat_impl(int olddirfd,
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

int sys_chmod_impl(const char* path, unsigned int mode) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    process_t* proc = (process_t*)sched_current();
    sys_stat_info_t info;
    int perm_rc;

    if (path_rc < 0) return path_rc;
    if (!proc) return -EINVAL;
    if (virtual_stat_info(kpath, &info)) {
        if (!process_is_root(proc) && proc->euid != info.uid) return -EPERM;
        return 0;
    }
    if (path_on_pseudo_mount(kpath)) return -EACCES;
    perm_rc = check_path_permission(proc, kpath, 0, &info);
    if (perm_rc < 0) return perm_rc;
    if (!process_is_root(proc) && proc->euid != info.uid) return -EPERM;
    return vfs_chmod(kpath, (u16)mode) ? 0 : path_lookup_errno(kpath);
}

int sys_chown_impl(const char* path, unsigned int uid, unsigned int gid) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    process_t* proc = (process_t*)sched_current();
    int perm_rc;

    if (path_rc < 0) return path_rc;
    if (!proc) return -EINVAL;
    if (virtual_path_exists(kpath, 0, 0, 0)) {
        if (!process_is_root(proc)) return -EPERM;
        return 0;
    }
    if (path_on_pseudo_mount(kpath)) return -EACCES;
    if (!process_is_root(proc)) return -EPERM;
    perm_rc = check_path_permission(proc, kpath, 0, 0);
    if (perm_rc < 0) return perm_rc;
    return vfs_chown(kpath, (u16)uid, (u16)gid) ? 0 : path_lookup_errno(kpath);
}

int sys_utimens_impl(const char* path, const struct user_timespec* times) {
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

int sys_utimensat_impl(int dirfd,
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

int sys_mknod_impl(const char* path, unsigned int mode, unsigned int dev) {
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

int sys_ftruncate_impl(int fd, unsigned int size) {
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

int sys_fchmod_impl(int fd, unsigned int mode) {
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

int sys_fchown_impl(int fd, unsigned int uid, unsigned int gid) {
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

int sys_futimens_impl(int fd, const struct user_timespec* times) {
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

int sys_statfs_impl(const char* path, sys_statfs_t* out) {
    char kpath[PROCESS_FD_NAME_MAX];
    int path_rc = copy_user_path_resolved(kpath, sizeof(kpath), path);
    if (path_rc < 0) return path_rc;
    return sys_statfs_kpath_impl(kpath, out);
}

int sys_fstatfs_impl(int fd, sys_statfs_t* out) {
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

int sys_mount_impl(const char* source,
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

int sys_umount2_impl(const char* target, unsigned int flags) {
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


int sys_fsinfo_impl(sys_fsinfo_t* out_info) {
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

int sys_fsmap_impl(sys_fsmap_request_t* user_req) {
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


int sys_block_read_sector_impl(unsigned int lba, void* user_buf) {
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

int sys_getcwd_impl(char* buf, unsigned int size) {
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

int sys_chdir_impl(const char* path) {
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

int sys_fchdir_impl(int fd) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    if (ent->kind != PROCESS_HANDLE_KIND_FILE || !ent->is_dir) return -ENOTDIR;
    if (!virtual_path_is_dir(ent->name) && !vfs_is_dir(ent->name)) {
        return -ENOENT;
    }
    {
        int perm_rc = check_path_permission(proc, ent->name, SYS_PERM_X, 0);
        if (perm_rc < 0) return perm_rc;
    }
    k_memcpy(proc->cwd, ent->name, (k_size_t)k_strlen(ent->name) + 1u);
    return 0;
}

int sys_stat_impl(const char* path, unsigned int* out_size, int* out_is_dir) {
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

int sys_fstat_impl(int fd, unsigned int* out_size, int* out_is_dir) {
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

int sys_stat_full_impl(const char* path, sys_stat_info_t* out) {
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

int sys_lstat_full_impl(const char* path, sys_stat_info_t* out) {
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

int sys_fstatat_full_impl(int dirfd,
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

int sys_fstat_full_impl(int fd, sys_stat_info_t* out) {
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


int sys_fread_impl(int fd, char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -EINVAL;
    fd_entry_t* ent = process_fd_get(proc, fd);
    if (!ent) return -EBADF;
    return process_fd_read(ent, buf, len);
}
