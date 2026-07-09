#include "process.h"
#include <stddef.h>
#include "pmm.h"
#include "paging.h"
#include "kalloc.h"
#include "klib.h"
#include "terminal.h"
#include "scheduler.h"
#include "keyboard.h"
#include "timer.h"
#include "../drivers/tcp.h"
#include "uapi_poll.h"
#include "uapi_errno.h"
#include "uapi_syscall.h"
#include "vfs.h"
#include "socket.h"
#include "wait.h"
#include "random.h"
#include "syscall_internal.h"
#include "../drivers/display.h"
#include "input.h"

typedef char process_t_must_fit_in_one_frame[(sizeof(process_t) <= 4096u) ? 1 : -1];

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static process_t* s_foreground_reader = 0;
static u32 s_foreground_pgid = 0;
static u32 s_next_pid = 1;
#define PROCESS_REGISTRY_INITIAL 64u
#define PROCESS_REGISTRY_MAX     256u
static process_t** s_process_registry = 0;
static unsigned int s_process_registry_capacity = 0;
static volatile int s_terminal_interrupt_pending = 0;
static process_t* s_terminal_interrupt_target = 0;
static volatile int s_terminal_stop_pending = 0;
static process_t* s_terminal_stop_target = 0;
static process_t* s_raw_console_reader = 0;
static process_t* s_display_input_owner = 0;
static volatile process_t* s_detach_requested = 0;
static volatile int s_detach_allowed = 0;

#define PROCESS_TERMINATED_BY_CTRL_C 130
#define PROCESS_SIGINT  2
#define PROCESS_SIGPIPE 13
#define PROCESS_SIGTERM 15
#define PROCESS_SIGCONT 18
#define PROCESS_SIGSTOP 19
#define PROCESS_SIGTSTP 20
#define PROCESS_SIGTTIN 21
#define PROCESS_SIGTTOU 22
#define PROCESS_STOPPED_STATUS(signum) (0x10000 | ((signum) & 0xFF))

#define TERM_VINTR  0
#define TERM_VERASE 2
#define TERM_VEOF   4
#define TERM_VTIME  5
#define TERM_VMIN   6
#define TERM_VSUSP 10
#define TERM_LFLAG_ISIG   0000001u
#define TERM_LFLAG_ICANON 0000002u
#define TERM_LFLAG_ECHO   0000010u
#define TERM_OFLAG_OPOST  0000001u
#define TERM_OFLAG_ONLCR  0000004u

typedef struct special_wait_object {
    wait_queue_t read_waiters;
    unsigned int signal_mask;
    unsigned int pending_signals;
} special_wait_object_t;

#define PIPE_BUFFER_SIZE PAGE_SIZE

typedef struct pipe_object {
    wait_queue_t read_waiters;
    wait_queue_t write_waiters;
    u32 data_frame;
    unsigned int read_pos;
    unsigned int write_pos;
    unsigned int count;
    unsigned int read_refs;
    unsigned int write_refs;
} pipe_object_t;

typedef struct pty_buffer {
    wait_queue_t read_waiters;
    wait_queue_t write_waiters;
    u32 data_frame;
    unsigned int read_pos;
    unsigned int write_pos;
    unsigned int count;
} pty_buffer_t;

typedef struct pty_object {
    pty_buffer_t master_to_slave;
    pty_buffer_t slave_to_master;
    unsigned int master_refs;
    unsigned int slave_refs;
    unsigned int rows;
    unsigned int cols;
    u32 foreground_pgid;
    unsigned int output_prev_cr;
    sys_termios_t termios;
} pty_object_t;

typedef struct virtual_object {
    unsigned int refs;
    unsigned int type;
    u32 data_frame;
} virtual_object_t;

typedef struct kernel_signalfd_siginfo {
    u32 ssi_signo;
    u32 ssi_errno;
    u32 ssi_code;
    u32 ssi_pid;
    u32 ssi_uid;
    u32 ssi_fd;
    u32 ssi_tid;
    u32 ssi_band;
    u32 ssi_overrun;
    u32 ssi_trapno;
    int ssi_status;
    int ssi_int;
    unsigned long long ssi_ptr;
    unsigned long long ssi_utime;
    unsigned long long ssi_stime;
    unsigned long long ssi_addr;
    unsigned short ssi_addr_lsb;
    u8 pad[46];
} kernel_signalfd_siginfo_t;

static int process_group_force_exit(u32 pgid,
                                    int status,
                                    process_t* defer_current,
                                    int mark_terminal_interrupt);

static sys_termios_t s_console_termios;
static int s_console_termios_init = 0;

static void terminal_attr_init_default(sys_termios_t* tio) {
    if (!tio) return;
    k_memset(tio, 0, sizeof(*tio));
    tio->c_lflag = TERM_LFLAG_ECHO | TERM_LFLAG_ICANON | TERM_LFLAG_ISIG;
    tio->c_oflag = TERM_OFLAG_OPOST | TERM_OFLAG_ONLCR;
    tio->c_cc[TERM_VINTR] = 3u;
    tio->c_cc[TERM_VERASE] = 8u;
    tio->c_cc[TERM_VEOF] = 4u;
    tio->c_cc[TERM_VMIN] = 1u;
    tio->c_cc[TERM_VTIME] = 0u;
    tio->c_cc[TERM_VSUSP] = 26u;
}

static sys_termios_t* console_termios(void) {
    if (!s_console_termios_init) {
        terminal_attr_init_default(&s_console_termios);
        s_console_termios_init = 1;
    }
    return &s_console_termios;
}

static int process_registry_ensure(void) {
    if (s_process_registry) return 1;
    s_process_registry = (process_t**)kcalloc(PROCESS_REGISTRY_INITIAL, sizeof(process_t*));
    if (!s_process_registry) return 0;
    s_process_registry_capacity = PROCESS_REGISTRY_INITIAL;
    return 1;
}

static int process_registry_grow(void) {
    unsigned int new_capacity;
    process_t** new_registry;

    if (!process_registry_ensure()) return 0;
    if (s_process_registry_capacity >= PROCESS_REGISTRY_MAX) return 0;

    new_capacity = s_process_registry_capacity * 2u;
    if (new_capacity > PROCESS_REGISTRY_MAX) {
        new_capacity = PROCESS_REGISTRY_MAX;
    }

    new_registry = (process_t**)kcalloc(new_capacity, sizeof(process_t*));
    if (!new_registry) return 0;
    k_memcpy(new_registry,
             s_process_registry,
             s_process_registry_capacity * sizeof(process_t*));
    kfree(s_process_registry);
    s_process_registry = new_registry;
    s_process_registry_capacity = new_capacity;
    return 1;
}

static int process_registry_add(process_t* proc) {
    if (!proc || !process_registry_ensure()) return 0;

    for (;;) {
        for (unsigned int i = 0; i < s_process_registry_capacity; i++) {
            if (!s_process_registry[i]) {
                s_process_registry[i] = proc;
                return 1;
            }
        }
        if (!process_registry_grow()) break;
    }

    terminal_puts("process: registry full\n");
    return 0;
}

static void process_registry_remove(process_t* proc) {
    if (!proc || !s_process_registry) return;

    for (unsigned int i = 0; i < s_process_registry_capacity; i++) {
        if (s_process_registry[i] == proc) {
            s_process_registry[i] = 0;
            return;
        }
    }
}

static u32 process_alloc_pid(void) {
    u32 start = s_next_pid ? s_next_pid : 1u;

    if (!process_registry_ensure()) return 0;
    if (s_next_pid == 0u) s_next_pid = 1u;

    for (;;) {
        u32 pid = s_next_pid;
        if (pid == 0u) pid = 1u;
        s_next_pid = pid + 1u;
        if (s_next_pid == 0u) s_next_pid = 1u;
        if (!process_find_by_pid(pid)) return pid;
        if (s_next_pid == start) break;
    }

    for (u32 pid = 1u; pid != 0u; pid++) {
        if (!process_find_by_pid(pid)) {
            s_next_pid = pid + 1u;
            if (s_next_pid == 0u) s_next_pid = 1u;
            return pid;
        }
    }
    return 0;
}

static u32 page_floor(u32 value) {
    return value & ~(PAGE_SIZE - 1u);
}

static int process_vm_compatible(const process_vm_area_t* a,
                                 const process_vm_area_t* b) {
    if (!a || !b) return 0;
    if (a->end != b->start) return 0;
    if (a->prot != b->prot || a->max_prot != b->max_prot ||
        a->kind != b->kind || a->flags != b->flags ||
        a->file_size != b->file_size) {
        return 0;
    }
    if (!k_strcmp(a->path, b->path)) return 0;
    if (a->kind == PROCESS_VM_KIND_FILE_PRIVATE || a->kind == PROCESS_VM_KIND_ELF) {
        if (a->file_end != b->file_start) return 0;
        if (a->file_offset + (a->file_end - a->file_start) != b->file_offset) return 0;
    }
    return 1;
}

static void process_vm_merge(process_t* proc) {
    if (!proc || !proc->vm_areas) return;

    unsigned int i = 0;
    while (i + 1u < proc->vm_area_count) {
        process_vm_area_t* cur = &proc->vm_areas[i];
        process_vm_area_t* next = &proc->vm_areas[i + 1u];
        if (process_vm_compatible(cur, next)) {
            cur->end = next->end;
            cur->file_end = next->file_end;
            for (unsigned int j = i + 1u; j + 1u < proc->vm_area_count; j++) {
                proc->vm_areas[j] = proc->vm_areas[j + 1u];
            }
            proc->vm_area_count--;
            continue;
        }
        i++;
    }
}

int process_vm_init(process_t* proc) {
    if (!proc) return 0;
    if (proc->vm_areas) return 1;

    unsigned int bytes = PROCESS_VM_AREA_INITIAL * sizeof(process_vm_area_t);
    proc->vm_areas = (process_vm_area_t*)kcalloc(PROCESS_VM_AREA_INITIAL,
                                                 sizeof(process_vm_area_t));
    if (!proc->vm_areas) return 0;
    proc->vm_area_frame = 0;
    proc->vm_area_frames = PAGE_ALIGN(bytes) / PAGE_SIZE;
    proc->vm_area_capacity = PROCESS_VM_AREA_INITIAL;
    proc->vm_area_count = 0;
    return 1;
}

void process_vm_free(process_t* proc) {
    if (!proc) return;
    if (proc->vm_areas) kfree(proc->vm_areas);
    proc->vm_areas = 0;
    proc->vm_area_frame = 0;
    proc->vm_area_frames = 0;
    proc->vm_area_count = 0;
    proc->vm_area_capacity = 0;
}

void process_vm_clear(process_t* proc) {
    if (!proc) return;
    if (!proc->vm_areas && !process_vm_init(proc)) return;
    proc->vm_area_count = 0;
    if (proc->vm_areas && proc->vm_area_capacity) {
        k_memset(proc->vm_areas,
                 0,
                 proc->vm_area_capacity * sizeof(process_vm_area_t));
    }
}

static int process_vm_ensure_capacity(process_t* proc, unsigned int needed) {
    unsigned int new_capacity;
    unsigned int bytes;
    process_vm_area_t* new_areas;

    if (!proc) return 0;
    if (!proc->vm_areas && !process_vm_init(proc)) return 0;
    if (needed <= proc->vm_area_capacity) return 1;
    if (needed > PROCESS_VM_AREA_MAX) return 0;

    new_capacity = proc->vm_area_capacity ? proc->vm_area_capacity
                                          : PROCESS_VM_AREA_INITIAL;
    while (new_capacity < needed) {
        new_capacity *= 2u;
        if (new_capacity > PROCESS_VM_AREA_MAX) {
            new_capacity = PROCESS_VM_AREA_MAX;
            break;
        }
    }
    if (new_capacity < needed) return 0;

    new_areas = (process_vm_area_t*)kcalloc(new_capacity, sizeof(process_vm_area_t));
    if (!new_areas) return 0;
    if (proc->vm_area_count > 0) {
        k_memcpy(new_areas,
                 proc->vm_areas,
                 proc->vm_area_count * sizeof(process_vm_area_t));
    }
    kfree(proc->vm_areas);
    proc->vm_areas = new_areas;
    proc->vm_area_capacity = new_capacity;
    bytes = new_capacity * sizeof(process_vm_area_t);
    proc->vm_area_frames = PAGE_ALIGN(bytes) / PAGE_SIZE;
    proc->vm_area_frame = 0;
    return 1;
}

int process_vm_clone(process_t* dst, process_t* src) {
    if (!dst || !src) return 0;
    if (!process_vm_init(dst)) return 0;
    if (!process_vm_ensure_capacity(dst, src->vm_area_capacity)) return 0;
    if (src->vm_area_count > 0) {
        k_memcpy(dst->vm_areas,
                 src->vm_areas,
                 src->vm_area_count * sizeof(process_vm_area_t));
    }
    dst->vm_area_count = src->vm_area_count;
    return 1;
}

static int process_vm_find_index(process_t* proc, u32 addr) {
    if (!proc || !proc->vm_areas) return -1;
    for (unsigned int i = 0; i < proc->vm_area_count; i++) {
        if (addr >= proc->vm_areas[i].start && addr < proc->vm_areas[i].end) {
            return (int)i;
        }
    }
    return -1;
}

static process_vm_area_t* process_vm_lookup(process_t* proc, u32 addr) {
    int idx = process_vm_find_index(proc, addr);
    return idx >= 0 ? &proc->vm_areas[idx] : 0;
}

static int process_vm_insert_area(process_t* proc,
                                  const process_vm_area_t* area,
                                  int merge) {
    if (!proc || !area || area->start >= area->end) return 0;
    if (!proc->vm_areas && !process_vm_init(proc)) return 0;
    if (!process_vm_ensure_capacity(proc, proc->vm_area_count + 1u)) return 0;

    unsigned int pos = 0;
    while (pos < proc->vm_area_count && proc->vm_areas[pos].start < area->start) {
        pos++;
    }
    if (pos > 0 && proc->vm_areas[pos - 1u].end > area->start) return 0;
    if (pos < proc->vm_area_count && area->end > proc->vm_areas[pos].start) return 0;

    for (unsigned int i = proc->vm_area_count; i > pos; i--) {
        proc->vm_areas[i] = proc->vm_areas[i - 1u];
    }
    proc->vm_areas[pos] = *area;
    proc->vm_area_count++;
    if (merge) process_vm_merge(proc);
    return 1;
}

int process_vm_add(process_t* proc,
                   u32 start,
                   u32 end,
                   u32 prot,
                   u32 max_prot,
                   u32 kind,
                   u32 flags,
                   const char* path,
                   u32 file_start,
                   u32 file_end,
                   u32 file_offset,
                   u32 file_size) {
    process_vm_area_t area;

    if (!proc) return 0;
    if ((start & (PAGE_SIZE - 1u)) != 0u || (end & (PAGE_SIZE - 1u)) != 0u) return 0;
    if (start < USER_CODE_BASE || end > USER_STACK_TOP || start >= end) return 0;
    if ((prot & ~max_prot) != 0u) return 0;
    if ((max_prot & ~(PROCESS_VM_PROT_READ | PROCESS_VM_PROT_WRITE | PROCESS_VM_PROT_EXEC)) != 0u) {
        return 0;
    }
    if (!process_vm_range_free(proc, start, end)) return 0;

    k_memset(&area, 0, sizeof(area));
    area.start = start;
    area.end = end;
    area.prot = prot;
    area.max_prot = max_prot;
    area.kind = kind;
    area.flags = flags;
    area.file_start = file_start;
    area.file_end = file_end;
    area.file_offset = file_offset;
    area.file_size = file_size;
    if (path) k_strncpy(area.path, path, sizeof(area.path));
    return process_vm_insert_area(proc, &area, 1);
}

int process_vm_range_free(process_t* proc, u32 start, u32 end) {
    if (!proc || start >= end) return 0;
    if (!proc->vm_areas) return 1;
    for (unsigned int i = 0; i < proc->vm_area_count; i++) {
        process_vm_area_t* area = &proc->vm_areas[i];
        if (start < area->end && end > area->start) return 0;
    }
    return 1;
}

static int process_vm_split_at(process_t* proc, u32 addr) {
    if (!proc || !proc->vm_areas) return 0;
    if ((addr & (PAGE_SIZE - 1u)) != 0u) return 0;

    int idx = process_vm_find_index(proc, addr);
    if (idx < 0) return 1;

    process_vm_area_t* area = &proc->vm_areas[idx];
    if (addr == area->start || addr == area->end) return 1;
    if (!process_vm_ensure_capacity(proc, proc->vm_area_count + 1u)) return 0;

    process_vm_area_t right = *area;
    u32 delta = addr - area->start;
    right.start = addr;
    if (right.kind == PROCESS_VM_KIND_FILE_PRIVATE || right.kind == PROCESS_VM_KIND_ELF) {
        if (right.file_start < addr && right.file_end > addr) {
            right.file_offset += delta;
            right.file_start = addr;
        } else if (right.file_end <= addr) {
            right.file_start = addr;
            right.file_end = addr;
        }
    }
    area->end = addr;
    if (area->kind == PROCESS_VM_KIND_FILE_PRIVATE || area->kind == PROCESS_VM_KIND_ELF) {
        if (area->file_end > addr) area->file_end = addr;
    }

    for (unsigned int i = proc->vm_area_count; i > (unsigned int)idx + 1u; i--) {
        proc->vm_areas[i] = proc->vm_areas[i - 1u];
    }
    proc->vm_areas[idx + 1] = right;
    proc->vm_area_count++;
    return 1;
}

static int process_vm_split_needed(process_t* proc, u32 addr) {
    int idx;
    process_vm_area_t* area;

    if (!proc || !proc->vm_areas) return 0;
    if ((addr & (PAGE_SIZE - 1u)) != 0u) return 0;
    idx = process_vm_find_index(proc, addr);
    if (idx < 0) return 0;
    area = &proc->vm_areas[idx];
    return addr != area->start && addr != area->end;
}

int process_vm_remove_range(process_t* proc, u32 start, u32 end) {
    if (!proc || start >= end) return 0;
    if ((start & (PAGE_SIZE - 1u)) != 0u || (end & (PAGE_SIZE - 1u)) != 0u) return 0;

    if (!proc->vm_areas) return 1;
    if (!process_vm_split_at(proc, start) || !process_vm_split_at(proc, end)) return 0;

    for (u32 page = start; page < end; page += PAGE_SIZE) {
        (void)paging_unmap_user_page(proc->pd, page);
    }

    unsigned int out = 0;
    for (unsigned int i = 0; i < proc->vm_area_count; i++) {
        process_vm_area_t area = proc->vm_areas[i];
        if (area.start >= start && area.end <= end) {
            continue;
        }
        proc->vm_areas[out++] = area;
    }
    proc->vm_area_count = out;
    process_vm_merge(proc);
    return 1;
}

int process_vm_remove_range_errno(process_t* proc, u32 start, u32 end) {
    unsigned int needed;

    if (!proc || start >= end) return -EINVAL;
    if ((start & (PAGE_SIZE - 1u)) != 0u || (end & (PAGE_SIZE - 1u)) != 0u) {
        return -EINVAL;
    }
    if (!proc->vm_areas) return 0;

    needed = proc->vm_area_count;
    if (process_vm_split_needed(proc, start)) needed++;
    if (process_vm_split_needed(proc, end)) needed++;
    if (!process_vm_ensure_capacity(proc, needed)) return -ENOMEM;
    return process_vm_remove_range(proc, start, end) ? 0 : -EINVAL;
}

static int process_vm_range_covered(process_t* proc, u32 start, u32 end) {
    u32 pos = start;
    if (!proc || !proc->vm_areas || start >= end) return 0;
    while (pos < end) {
        process_vm_area_t* area = process_vm_lookup(proc, pos);
        if (!area || area->start > pos) return 0;
        pos = area->end;
    }
    return 1;
}

int process_vm_protect(process_t* proc, u32 start, u32 end, u32 prot) {
    if (!proc || start >= end) return 0;
    if ((start & (PAGE_SIZE - 1u)) != 0u || (end & (PAGE_SIZE - 1u)) != 0u) return 0;
    if ((prot & ~(PROCESS_VM_PROT_READ | PROCESS_VM_PROT_WRITE | PROCESS_VM_PROT_EXEC)) != 0u) {
        return 0;
    }
    if (!process_vm_range_covered(proc, start, end)) return 0;

    for (unsigned int i = 0; i < proc->vm_area_count; i++) {
        process_vm_area_t* area = &proc->vm_areas[i];
        if (area->end <= start || area->start >= end) continue;
        if ((prot & ~area->max_prot) != 0u) return 0;
    }

    if (!process_vm_split_at(proc, start) || !process_vm_split_at(proc, end)) return 0;

    for (unsigned int i = 0; i < proc->vm_area_count; i++) {
        process_vm_area_t* area = &proc->vm_areas[i];
        if (area->end <= start || area->start >= end) continue;
        area->prot = prot;
        for (u32 page = area->start; page < area->end; page += PAGE_SIZE) {
            u32 phys = 0;
            u32 flags = 0;
            if (!paging_user_page_get(proc->pd, page, &phys, &flags)) continue;
            if (prot & PROCESS_VM_PROT_WRITE) {
                if ((flags & PAGE_COW) == 0u && (flags & PAGE_SHARED_RO_FILE) == 0u) {
                    (void)paging_user_page_set_flags(proc->pd, page, 0, PAGE_WRITE);
                }
            } else {
                (void)paging_user_page_set_flags(proc->pd, page, PAGE_WRITE, 0);
            }
        }
    }

    process_vm_merge(proc);
    return 1;
}

int process_vm_protect_errno(process_t* proc, u32 start, u32 end, u32 prot) {
    unsigned int needed;

    if (!proc || start >= end) return -EINVAL;
    if ((start & (PAGE_SIZE - 1u)) != 0u || (end & (PAGE_SIZE - 1u)) != 0u) {
        return -EINVAL;
    }
    if ((prot & ~(PROCESS_VM_PROT_READ | PROCESS_VM_PROT_WRITE | PROCESS_VM_PROT_EXEC)) != 0u) {
        return -EINVAL;
    }
    if (!process_vm_range_covered(proc, start, end)) return -EINVAL;

    needed = proc->vm_area_count;
    if (process_vm_split_needed(proc, start)) needed++;
    if (process_vm_split_needed(proc, end)) needed++;
    if (!process_vm_ensure_capacity(proc, needed)) return -ENOMEM;
    return process_vm_protect(proc, start, end, prot) ? 0 : -EINVAL;
}

static int process_vm_load_file_page(process_vm_area_t* area, u32 page, u32 frame) {
    u32 copy_start;
    u32 copy_end;
    u32 read = 0;

    if (!area || area->path[0] == '\0') return 0;
    if (area->file_end <= page || area->file_start >= page + PAGE_SIZE) return 1;

    copy_start = area->file_start > page ? area->file_start : page;
    copy_end = area->file_end < page + PAGE_SIZE ? area->file_end : page + PAGE_SIZE;
    if (copy_end <= copy_start) return 1;

    if (vfs_read_path_at(area->path,
                         area->file_offset + (copy_start - area->file_start),
                         (u8*)paging_phys_to_kernel_virt(frame) + (copy_start - page),
                         copy_end - copy_start,
                         &read) < 0) {
        return 0;
    }
    return read == copy_end - copy_start;
}

int process_vm_handle_fault(process_t* proc, u32 fault_addr, u32 err) {
    if (!proc || !proc->pd) return 0;

    u32 page = page_floor(fault_addr);
    int fault_present = (err & 0x1u) != 0u;
    int fault_write = (err & 0x2u) != 0u;
    process_vm_area_t* area = process_vm_lookup(proc, page);
    if (!area) return 0;

    if (fault_write && (area->prot & PROCESS_VM_PROT_WRITE) == 0u) return 0;
    if (!fault_write &&
        (area->prot & (PROCESS_VM_PROT_READ | PROCESS_VM_PROT_WRITE | PROCESS_VM_PROT_EXEC)) == 0u) {
        return 0;
    }

    if (fault_present) {
        u32 phys = 0;
        u32 flags = 0;
        if (!fault_write) return 0;
        if (!paging_user_page_get(proc->pd, page, &phys, &flags)) return 0;
        if ((flags & PAGE_COW) == 0u) return 0;
        return paging_resolve_cow_fault(proc->pd, page);
    }

    if (paging_user_page_present(proc->pd, page)) return 0;

    if ((area->kind == PROCESS_VM_KIND_FILE_PRIVATE || area->kind == PROCESS_VM_KIND_ELF) &&
        area->path[0] != '\0' &&
        (area->max_prot & PROCESS_VM_PROT_WRITE) == 0u &&
        (area->prot & PROCESS_VM_PROT_WRITE) == 0u &&
        area->file_start <= page &&
        area->file_end >= page + PAGE_SIZE) {
        u32 frame = 0;
        u32 bytes = 0;
        u32 file_off = area->file_offset + (page - area->file_start);
        if ((file_off & (PAGE_SIZE - 1u)) == 0u &&
            vfs_file_map_ro_page_path(area->path, file_off, &frame, &bytes) == 0) {
            (void)bytes;
            paging_map_page(proc->pd, page, frame, PAGE_USER | PAGE_SHARED_RO_FILE);
            return 1;
        }
    }

    u32 frame = pmm_alloc_frame();
    if (!frame) return 0;
    k_memset(paging_phys_to_kernel_virt(frame), 0, PAGE_SIZE);
    if (area->kind == PROCESS_VM_KIND_FILE_PRIVATE || area->kind == PROCESS_VM_KIND_ELF) {
        if (!process_vm_load_file_page(area, page, frame)) {
            pmm_release_frame(frame);
            return 0;
        }
    }

    u32 map_flags = PAGE_USER;
    if (area->prot & PROCESS_VM_PROT_WRITE) {
        map_flags |= PAGE_WRITE;
    } else if (area->max_prot & PROCESS_VM_PROT_WRITE) {
        map_flags |= PAGE_COW;
    }
    paging_map_page(proc->pd, page, frame, map_flags);
    return 1;
}

int process_vm_fault_in_page(process_t* proc, u32 addr, int write) {
    u32 flags = 0;
    u32 phys = 0;
    u32 err = write ? 0x2u : 0u;

    if (!proc || !proc->pd) return 0;
    if (paging_user_page_get(proc->pd, addr, &phys, &flags)) {
        if (!write) return 1;
        if (flags & PAGE_WRITE) return 1;
        err |= 0x1u;
    }
    return process_vm_handle_fault(proc, addr, err);
}

int process_vm_page_may_write(void* ctx, u32 virt) {
    process_t* proc = (process_t*)ctx;
    process_vm_area_t* area = process_vm_lookup(proc, page_floor(virt));
    return area && (area->max_prot & PROCESS_VM_PROT_WRITE);
}

process_t* process_find_by_pid(u32 pid) {
    if (pid == 0) return 0;
    if (!s_process_registry) return 0;

    for (unsigned int i = 0; i < s_process_registry_capacity; i++) {
        process_t* proc = s_process_registry[i];
        if (proc && proc->pid == pid) {
            return proc;
        }
    }

    return 0;
}

process_t* process_find_by_pgid(u32 pgid) {
    if (pgid == 0) return 0;
    if (!s_process_registry) return 0;

    for (unsigned int i = 0; i < s_process_registry_capacity; i++) {
        process_t* proc = s_process_registry[i];
        if (proc && proc->pgid == pgid) {
            return proc;
        }
    }

    return 0;
}

void process_accounting_snapshot(process_accounting_t* out) {
    if (!out) return;
    k_memset(out, 0, sizeof(*out));
    out->process_capacity = s_process_registry_capacity;
    if (!s_process_registry) return;

    for (unsigned int i = 0; i < s_process_registry_capacity; i++) {
        process_t* proc = s_process_registry[i];
        if (!proc) continue;

        out->process_count++;
        out->process_pages += 1u;
        out->kernel_stack_pages += proc->kernel_stack_frames;
        out->fd_table_pages += proc->fd_table_frames;
        out->vm_area_pages += proc->vm_area_frames;
        out->private_mapping_pages += process_pd_count_private_frames(proc->pd);
        if (proc->heap_brk >= proc->heap_base) {
            out->heap_bytes += proc->heap_brk - proc->heap_base;
        }
    }
}

void process_wake_parent_waiter(process_t* child) {
    process_t* parent;

    if (!child || child->parent_pid == 0) return;
    parent = process_find_by_pid(child->parent_pid);
    if (!parent) return;
    if (parent->state == PROCESS_STATE_WAITING) {
        parent->state = PROCESS_STATE_RUNNING;
        parent->sleep_until = 0u;
    }
}

static process_t* process_find_child(process_t* parent, int pid) {
    if (!parent) return 0;
    if (!s_process_registry) return 0;

    for (unsigned int i = 0; i < s_process_registry_capacity; i++) {
        process_t* proc = s_process_registry[i];
        if (!proc || proc->parent_pid != parent->pid) {
            continue;
        }
        if (pid == -1 || proc->pid == (u32)pid) {
            return proc;
        }
    }

    return 0;
}

static process_t* process_find_zombie_child(process_t* parent, int pid) {
    if (!parent) return 0;
    if (!s_process_registry) return 0;

    for (unsigned int i = 0; i < s_process_registry_capacity; i++) {
        process_t* proc = s_process_registry[i];
        if (!proc || proc->parent_pid != parent->pid) {
            continue;
        }
        if (pid != -1 && proc->pid != (u32)pid) {
            continue;
        }
        if (proc->state == PROCESS_STATE_ZOMBIE) {
            return proc;
        }
    }

    return 0;
}

static process_t* process_find_stopped_child(process_t* parent, int pid) {
    if (!parent) return 0;
    if (!s_process_registry) return 0;

    for (unsigned int i = 0; i < s_process_registry_capacity; i++) {
        process_t* proc = s_process_registry[i];
        if (!proc || proc->parent_pid != parent->pid) {
            continue;
        }
        if (pid != -1 && proc->pid != (u32)pid) {
            continue;
        }
        if (proc->state == PROCESS_STATE_STOPPED && !proc->stop_reported) {
            return proc;
        }
    }

    return 0;
}

static void process_orphan_children(u32 parent_pid) {
    if (parent_pid == 0) return;
    if (!s_process_registry) return;

    for (unsigned int i = 0; i < s_process_registry_capacity; i++) {
        process_t* proc = s_process_registry[i];
        if (proc && proc->parent_pid == parent_pid) {
            proc->parent_pid = 0;
            proc->reaper_claimed = 0;
        }
    }
}

static void process_clear_display_input_owner(process_t* proc) {
    if (!proc || s_display_input_owner != proc) {
        return;
    }

    s_display_input_owner = 0;
    keyboard_buf_clear();
    input_clear_events();
}

/*
 * process_key_consumer — keyboard consumer active while a user process
 * holds the foreground.  Ctrl+C is handled as a terminal interrupt for
 * the foreground process group; ordinary ASCII is buffered for SYS_READ.
 *
 * True-blocking SYS_READ wake-up:
 *   After pushing the character into kb_buf, check whether a process is
 *   parked in PROCESS_STATE_WAITING.  If so, mark it PROCESS_STATE_RUNNING
 *   and clear the waiting slot so the scheduler will pick it up on the
 *   next pass.
 *
 *   This runs entirely in IRQ1 context (IF=0).  It must not allocate or
 *   block; if Ctrl+C interrupts the currently running process, it records
 *   a pending terminal interrupt and lets irq1_handler_main switch away
 *   after keyboard_handle_irq() returns with the saved IRQ frame ESP.
 */
static void process_key_consumer(key_event_t ev) {
    if (ev.ctrl && ev.key == KEY_C) {
        u32 pgid = s_foreground_pgid;
        int defaulted;
        sys_termios_t* tio = console_termios();

        if ((tio->c_lflag & TERM_LFLAG_ISIG) == 0u ||
            (s_raw_console_reader && s_raw_console_reader == s_foreground_reader)) {
            process_t* waiter;
            keyboard_buf_push_char((char)(tio->c_cc[TERM_VINTR] ? tio->c_cc[TERM_VINTR] : 3u));
            waiter = (process_t*)keyboard_get_waiting_process();
            if (waiter && waiter->state == PROCESS_STATE_WAITING) {
                waiter->state = PROCESS_STATE_RUNNING;
                keyboard_set_waiting_process(0);
            }
            return;
        }

        if (pgid == 0) return;

        keyboard_buf_clear();
        if (process_group_signal_deliver(pgid, PROCESS_SIGINT)) {
            return;
        }

        defaulted = process_group_force_exit(pgid,
                                             PROCESS_TERMINATED_BY_CTRL_C,
                                             sched_current(),
                                             1);
        if (defaulted) {
            process_set_foreground(0);
            terminal_puts("^C\n");
        }
        return;
    }

    if (!s_foreground_reader || s_foreground_pgid == 0) {
        return;
    }

    if (ev.ctrl && ev.key == KEY_Z) {
        process_t* proc = s_foreground_reader;
        u32 pgid = s_foreground_pgid;
        if (!proc) return;

        terminal_puts("^Z\n");
        keyboard_buf_clear();
        if (s_detach_allowed) {
            s_detach_requested = proc;
            s_foreground_reader = 0;
            s_foreground_pgid = 0;
            process_wake_parent_waiter(proc);
        } else if (pgid != 0u) {
            (void)process_group_stop(pgid, PROCESS_SIGTSTP, sched_current(), 1);
            process_set_foreground(0);
        }
        return;
    }

    if (!ev.ascii) {
        const char* seq = 0;

        switch (ev.key) {
            case KEY_UP:       seq = "\x1b[A"; break;
            case KEY_DOWN:     seq = "\x1b[B"; break;
            case KEY_RIGHT:    seq = "\x1b[C"; break;
            case KEY_LEFT:     seq = "\x1b[D"; break;
            case KEY_ESC:      seq = "\x1b"; break;
            case KEY_HOME:     seq = "\x1b[H"; break;
            case KEY_END:      seq = "\x1b[F"; break;
            case KEY_INSERT:   seq = "\x1b[2~"; break;
            case KEY_DELETE:   seq = "\x1b[3~"; break;
            case KEY_PAGEUP:   seq = "\x1b[5~"; break;
            case KEY_PAGEDOWN: seq = "\x1b[6~"; break;
            case KEY_F1:       seq = "\x1bOP"; break;
            case KEY_F2:       seq = "\x1bOQ"; break;
            case KEY_F3:       seq = "\x1bOR"; break;
            case KEY_F4:       seq = "\x1bOS"; break;
            case KEY_F10:      seq = "\x1b[21~"; break;
            default: break;
        }

        if (!seq) return;
        for (int i = 0; seq[i] != '\0'; i++) {
            keyboard_buf_push_char(seq[i]);
        }

        process_t* waiter = (process_t*)keyboard_get_waiting_process();
        if (waiter && waiter->state == PROCESS_STATE_WAITING) {
            waiter->state = PROCESS_STATE_RUNNING;
            keyboard_set_waiting_process(0);
        }
        return;
    }

    keyboard_buf_push_char(ev.ascii);

    process_t* waiter = (process_t*)keyboard_get_waiting_process();
    if (waiter && waiter->state == PROCESS_STATE_WAITING) {
        waiter->state = PROCESS_STATE_RUNNING;
        keyboard_set_waiting_process(0);
    }
}

static void proc_zero(process_t* p) {
    unsigned char* b = (unsigned char*)p;
    for (unsigned int i = 0; i < sizeof(process_t); i++) b[i] = 0;
}

static void str_copy_n(char* dst, const char* src, unsigned int n) {
    unsigned int i = 0;
    for (; i < n - 1 && src[i]; i++) dst[i] = src[i];
    dst[i] = '\0';
}

static unsigned int process_fd_frame_count(unsigned int capacity) {
    unsigned int bytes = capacity * (unsigned int)sizeof(fd_entry_t);
    return PAGE_ALIGN(bytes) / PAGE_SIZE;
}

static int process_fd_table_alloc(unsigned int capacity,
                                  fd_entry_t** out_fds,
                                  u32* out_frame,
                                  u32* out_frames) {
    u32 frames;
    u32 frame;
    fd_entry_t* fds;

    if (!out_fds || !out_frame || !out_frames) return -EINVAL;
    if (capacity == 0u || capacity > PROCESS_FD_LIMIT_HARD) return -EINVAL;

    frames = process_fd_frame_count(capacity);
    frame = pmm_alloc_contiguous_frames(frames);
    if (!frame) return -ENOMEM;

    fds = (fd_entry_t*)paging_phys_to_kernel_virt(frame);
    k_memset(fds, 0, frames * PAGE_SIZE);

    *out_fds = fds;
    *out_frame = frame;
    *out_frames = frames;
    return 0;
}

static int process_fd_table_is_valid(process_t* proc) {
    u32 expected_virt;

    if (!proc || !proc->fds) return 0;
    if (proc->fd_capacity == 0u ||
        proc->fd_capacity > PROCESS_FD_LIMIT_HARD ||
        proc->fd_limit == 0u ||
        proc->fd_limit > PROCESS_FD_LIMIT_HARD ||
        proc->fd_capacity > proc->fd_limit) {
        return 0;
    }
    if (!proc->fd_table_frame || !proc->fd_table_frames) {
        return 0;
    }
    if (!paging_phys_is_pmm_frame(proc->fd_table_frame)) {
        return 0;
    }
    if (proc->fd_table_frames < process_fd_frame_count(proc->fd_capacity)) {
        return 0;
    }

    expected_virt = KERNEL_PMM_MAP_BASE + (proc->fd_table_frame - PMM_BASE);
    return (u32)proc->fds == expected_virt;
}

static void process_fd_table_free(process_t* proc) {
    if (!proc || !proc->fd_table_frame || !proc->fd_table_frames) return;

    proc->fds = 0;
    proc->fd_capacity = 0;
    proc->fd_limit = 0;
    pmm_free_contiguous_frames(proc->fd_table_frame, proc->fd_table_frames);
    proc->fd_table_frame = 0;
    proc->fd_table_frames = 0;
}

static int process_string_arena_ensure(char** out_data,
                                       u32* out_frame,
                                       u32* out_frames,
                                       unsigned int bytes) {
    u32 frames;
    u32 frame;
    char* data;

    if (!out_data || !out_frame || !out_frames || bytes == 0u) return -EINVAL;
    if (*out_data) return 0;

    frames = PAGE_ALIGN(bytes) / PAGE_SIZE;
    frame = pmm_alloc_contiguous_frames(frames);
    if (!frame) return -ENOMEM;

    data = (char*)paging_phys_to_kernel_virt(frame);
    k_memset(data, 0, frames * PAGE_SIZE);

    *out_data = data;
    *out_frame = frame;
    *out_frames = frames;
    return 0;
}

static void process_string_arena_free(char** data, u32* frame, u32* frames) {
    if (!data || !frame || !frames) return;
    if (*frame && *frames) {
        pmm_free_contiguous_frames(*frame, *frames);
    }
    *data = 0;
    *frame = 0;
    *frames = 0;
}

static void process_launch_context_free(process_t* proc) {
    if (!proc) return;

    proc->user_argc = 0;
    for (int i = 0; i <= PROCESS_MAX_ARGS; i++) proc->user_argv[i] = 0;
    process_string_arena_free(&proc->user_arg_data,
                              &proc->user_arg_frame,
                              &proc->user_arg_frames);

    proc->user_envc = 0;
    for (int i = 0; i <= PROCESS_MAX_ENVS; i++) proc->user_envp[i] = 0;
    process_string_arena_free(&proc->user_env_data,
                              &proc->user_env_frame,
                              &proc->user_env_frames);
}

static int process_fd_table_init(process_t* proc, unsigned int limit) {
    fd_entry_t* fds = 0;
    u32 frame = 0;
    u32 frames = 0;
    unsigned int capacity = PROCESS_FD_INITIAL_CAPACITY;
    int rc;

    if (!proc) return -EINVAL;
    if (limit < PROCESS_FD_INITIAL_CAPACITY) {
        limit = PROCESS_FD_INITIAL_CAPACITY;
    }
    if (limit > PROCESS_FD_LIMIT_HARD) {
        limit = PROCESS_FD_LIMIT_HARD;
    }
    if (capacity > limit) {
        capacity = limit;
    }

    rc = process_fd_table_alloc(capacity, &fds, &frame, &frames);
    if (rc < 0) return rc;

    proc->fds = fds;
    proc->fd_capacity = capacity;
    proc->fd_limit = limit;
    proc->fd_table_frame = frame;
    proc->fd_table_frames = frames;
    return 0;
}

static int process_fd_table_grow(process_t* proc, unsigned int min_capacity) {
    fd_entry_t* new_fds = 0;
    u32 new_frame = 0;
    u32 new_frames = 0;
    u32 old_frame;
    u32 old_frames;
    unsigned int new_capacity;
    int rc;

    if (!proc || !proc->fds) return -EINVAL;
    if (min_capacity <= proc->fd_capacity) return 0;
    if (proc->fd_capacity >= proc->fd_limit) return -ENFILE;
    if (min_capacity > proc->fd_limit) return -ENFILE;

    new_capacity = proc->fd_capacity * 2u;
    if (new_capacity < min_capacity) {
        new_capacity = min_capacity;
    }
    if (new_capacity > proc->fd_limit) {
        new_capacity = proc->fd_limit;
    }

    rc = process_fd_table_alloc(new_capacity, &new_fds, &new_frame, &new_frames);
    if (rc < 0) return rc;

    k_memcpy(new_fds, proc->fds, proc->fd_capacity * (unsigned int)sizeof(fd_entry_t));
    old_frame = proc->fd_table_frame;
    old_frames = proc->fd_table_frames;

    proc->fds = new_fds;
    proc->fd_table_frame = new_frame;
    proc->fd_table_frames = new_frames;
    proc->fd_capacity = new_capacity;
    pmm_free_contiguous_frames(old_frame, old_frames);
    return 0;
}

static int process_fd_alloc_entry(process_t* proc, fd_entry_t** out_ent) {
    int rc;

    if (!proc || !out_ent) return -EINVAL;
    if (!proc->fds || proc->fd_capacity <= PROCESS_FD_FIRST) return -EINVAL;

    for (;;) {
        for (unsigned int fd = PROCESS_FD_FIRST; fd < proc->fd_capacity; fd++) {
            if (!proc->fds[fd].valid) {
                *out_ent = &proc->fds[fd];
                return (int)fd;
            }
        }

        if (proc->fd_capacity >= proc->fd_limit) {
            return -ENFILE;
        }

        rc = process_fd_table_grow(proc, proc->fd_capacity + 1u);
        if (rc < 0) return rc;
    }
}

static int process_fd_alloc_exact(process_t* proc, int fd, fd_entry_t** out_ent) {
    int rc;

    if (!proc || !out_ent) return -EINVAL;
    if (fd < 0 || (unsigned int)fd >= proc->fd_limit) return -EBADF;

    while ((unsigned int)fd >= proc->fd_capacity) {
        rc = process_fd_table_grow(proc, (unsigned int)fd + 1u);
        if (rc < 0) return rc;
    }

    if (proc->fds[fd].valid) {
        process_fd_close(&proc->fds[fd]);
    }
    *out_ent = &proc->fds[fd];
    return fd;
}

static int process_handle_console_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_console_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_console_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_console_poll(fd_entry_t* ent, short events);
static void process_handle_console_close(fd_entry_t* ent);

static int process_handle_socket_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_socket_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_socket_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_socket_poll(fd_entry_t* ent, short events);
static void process_handle_socket_close(fd_entry_t* ent);

static int process_handle_special_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_special_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_special_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_special_poll(fd_entry_t* ent, short events);
static void process_handle_special_close(fd_entry_t* ent);

static int process_handle_pipe_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_pipe_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_pipe_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_pipe_poll(fd_entry_t* ent, short events);
static void process_handle_pipe_close(fd_entry_t* ent);

static int process_handle_pty_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_pty_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_pty_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_pty_poll(fd_entry_t* ent, short events);
static void process_handle_pty_close(fd_entry_t* ent);

static int process_handle_virtual_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_virtual_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_virtual_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_virtual_poll(fd_entry_t* ent, short events);
static void process_handle_virtual_close(fd_entry_t* ent);

static const process_handle_ops_t s_socket_handle_ops = {
    .read = process_handle_socket_read,
    .write = process_handle_socket_write,
    .seek = process_handle_socket_seek,
    .poll = process_handle_socket_poll,
    .flush = 0,
    .close = process_handle_socket_close,
};

static const process_handle_ops_t s_special_handle_ops = {
    .read = process_handle_special_read,
    .write = process_handle_special_write,
    .seek = process_handle_special_seek,
    .poll = process_handle_special_poll,
    .flush = 0,
    .close = process_handle_special_close,
};

static const process_handle_ops_t s_console_handle_ops = {
    .read = process_handle_console_read,
    .write = process_handle_console_write,
    .seek = process_handle_console_seek,
    .poll = process_handle_console_poll,
    .flush = 0,
    .close = process_handle_console_close,
};

static const process_handle_ops_t s_pipe_handle_ops = {
    .read = process_handle_pipe_read,
    .write = process_handle_pipe_write,
    .seek = process_handle_pipe_seek,
    .poll = process_handle_pipe_poll,
    .flush = 0,
    .close = process_handle_pipe_close,
};

static const process_handle_ops_t s_pty_handle_ops = {
    .read = process_handle_pty_read,
    .write = process_handle_pty_write,
    .seek = process_handle_pty_seek,
    .poll = process_handle_pty_poll,
    .flush = 0,
    .close = process_handle_pty_close,
};

static const process_handle_ops_t s_virtual_handle_ops = {
    .read = process_handle_virtual_read,
    .write = process_handle_virtual_write,
    .seek = process_handle_virtual_seek,
    .poll = process_handle_virtual_poll,
    .flush = 0,
    .close = process_handle_virtual_close,
};

static void process_init_standard_fds(process_t* proc) {
    if (!proc) return;
    if (!proc->fds || proc->fd_capacity < PROCESS_FD_FIRST) return;

    proc->fds[0].valid = 1;
    proc->fds[0].kind = PROCESS_HANDLE_KIND_CONSOLE;
    proc->fds[0].ops = &s_console_handle_ops;
    proc->fds[0].readable = 1;
    proc->fds[0].writable = 0;

    for (int fd = 1; fd <= 2; fd++) {
        proc->fds[fd].valid = 1;
        proc->fds[fd].kind = PROCESS_HANDLE_KIND_CONSOLE;
        proc->fds[fd].ops = &s_console_handle_ops;
        proc->fds[fd].readable = 0;
        proc->fds[fd].writable = 1;
    }
}

fd_entry_t* process_fd_get(process_t* proc, int fd) {
    if (!proc) return 0;
    if (!proc->fds) return 0;
    if (fd < 0 || (unsigned int)fd >= proc->fd_capacity) return 0;
    if (!proc->fds[fd].valid) return 0;
    return &proc->fds[fd];
}

int process_fd_open_file_mode(process_t* proc,
                              const char* name,
                              u32 size,
                              int readable,
                              int writable) {
    fd_entry_t* ent;
    int fd;

    if (!proc || !name) return -EINVAL;

    fd = process_fd_alloc_entry(proc, &ent);
    if (fd < 0) return fd;

    k_memset(ent, 0, sizeof(*ent));
    ent->valid = 1;
    {
        int rc = vfs_file_init(ent, name, size, readable, writable);
        if (rc < 0) {
            k_memset(ent, 0, sizeof(*ent));
            return rc;
        }
    }
    return fd;
}

int process_fd_open_file(process_t* proc, const char* name, u32 size, int writable) {
    return process_fd_open_file_mode(proc, name, size, writable ? 0 : 1, writable);
}

int process_fd_open_socket_kind(process_t* proc, const char* name, int socket_kind) {
    fd_entry_t* ent;
    socket_t* sock;
    int fd;

    if (!proc) return -EINVAL;

    fd = process_fd_alloc_entry(proc, &ent);
    if (fd < 0) return fd;

    sock = socket_create_kind((socket_kind_t)socket_kind);
    if (!sock) return -ENOMEM;

    k_memset(ent, 0, sizeof(*ent));
    ent->valid = 1;
    ent->kind = PROCESS_HANDLE_KIND_SOCKET;
    ent->ops = &s_socket_handle_ops;
    ent->socket = sock;
    ent->socket_state = PROCESS_SOCKET_STATE_OPEN;
    ent->socket_port = 0;
    ent->socket_conn = TCP_SOCKET_CONN_NONE;
    ent->readable = 1;
    ent->writable = 0;
    if (name) {
        k_memcpy(ent->name, name, (k_size_t)k_strlen(name) + 1u);
    }
    return fd;
}

int process_fd_open_socket(process_t* proc, const char* name) {
    return process_fd_open_socket_kind(proc, name, SOCKET_KIND_TCP);
}

int process_fd_open_special(process_t* proc, int kind, const char* name) {
    fd_entry_t* ent;
    int fd;

    if (!proc) return -EINVAL;
    if (kind != PROCESS_HANDLE_KIND_EPOLL &&
        kind != PROCESS_HANDLE_KIND_TIMERFD &&
        kind != PROCESS_HANDLE_KIND_SIGNALFD) {
        return -EINVAL;
    }

    fd = process_fd_alloc_entry(proc, &ent);
    if (fd < 0) return fd;

    k_memset(ent, 0, sizeof(*ent));
    ent->valid = 1;
    ent->kind = kind;
    ent->ops = &s_special_handle_ops;
    ent->readable = 1;
    ent->writable = 0;
    if (name) {
        k_memcpy(ent->name, name, (k_size_t)k_strlen(name) + 1u);
    }
    return fd;
}

static virtual_object_t* virtual_object_from_ent(fd_entry_t* ent) {
    if (!ent || ent->kind != PROCESS_HANDLE_KIND_VIRTUAL || !ent->aux_frame) return 0;
    return (virtual_object_t*)paging_phys_to_kernel_virt(ent->aux_frame);
}

static int virtual_entry_is_tty(fd_entry_t* ent) {
    virtual_object_t* obj = virtual_object_from_ent(ent);
    return obj && obj->type == PROCESS_VIRTUAL_TTY;
}

int process_fd_open_virtual(process_t* proc,
                            const char* name,
                            unsigned int type,
                            const char* data,
                            unsigned int size,
                            int readable,
                            int writable) {
    fd_entry_t* ent;
    virtual_object_t* obj;
    u32 obj_frame;
    int fd;

    if (!proc || !name) return -EINVAL;
    if (size > PAGE_SIZE) return -EFBIG;
    if (type != PROCESS_VIRTUAL_REGULAR &&
        type != PROCESS_VIRTUAL_NULL &&
        type != PROCESS_VIRTUAL_ZERO &&
        type != PROCESS_VIRTUAL_TTY &&
        type != PROCESS_VIRTUAL_URANDOM) {
        return -EINVAL;
    }

    obj_frame = pmm_alloc_frame();
    if (!obj_frame) return -ENOMEM;
    obj = (virtual_object_t*)paging_phys_to_kernel_virt(obj_frame);
    k_memset(obj, 0, PAGE_SIZE);
    obj->refs = 1;
    obj->type = type;

    if (type == PROCESS_VIRTUAL_REGULAR && size > 0u) {
        obj->data_frame = pmm_alloc_frame();
        if (!obj->data_frame) {
            pmm_free_frame(obj_frame);
            return -ENOMEM;
        }
        k_memset(paging_phys_to_kernel_virt(obj->data_frame), 0, PAGE_SIZE);
        if (data) {
            k_memcpy(paging_phys_to_kernel_virt(obj->data_frame), data, size);
        }
    }

    fd = process_fd_alloc_entry(proc, &ent);
    if (fd < 0) {
        if (obj->data_frame) pmm_free_frame(obj->data_frame);
        pmm_free_frame(obj_frame);
        return fd;
    }

    k_memset(ent, 0, sizeof(*ent));
    ent->valid = 1;
    ent->kind = PROCESS_HANDLE_KIND_VIRTUAL;
    ent->ops = &s_virtual_handle_ops;
    ent->readable = readable ? 1 : 0;
    ent->writable = writable ? 1 : 0;
    ent->aux_frame = obj_frame;
    ent->size = size;
    ent->offset = 0;
    k_strncpy(ent->name, name, sizeof(ent->name));
    return fd;
}

static pipe_object_t* pipe_object_from_ent(fd_entry_t* ent) {
    if (!ent || ent->kind != PROCESS_HANDLE_KIND_PIPE || !ent->aux_frame) return 0;
    return (pipe_object_t*)paging_phys_to_kernel_virt(ent->aux_frame);
}

static char* pipe_data(pipe_object_t* pipe) {
    if (!pipe || !pipe->data_frame) return 0;
    return (char*)paging_phys_to_kernel_virt(pipe->data_frame);
}

static pty_object_t* pty_object_from_ent(fd_entry_t* ent) {
    if (!ent || !ent->aux_frame) return 0;
    if (ent->kind != PROCESS_HANDLE_KIND_PTY_MASTER &&
        ent->kind != PROCESS_HANDLE_KIND_PTY_SLAVE) {
        return 0;
    }
    return (pty_object_t*)paging_phys_to_kernel_virt(ent->aux_frame);
}

static char* pty_buffer_data(pty_buffer_t* buffer) {
    if (!buffer || !buffer->data_frame) return 0;
    return (char*)paging_phys_to_kernel_virt(buffer->data_frame);
}

static void process_fd_pipe_ref(fd_entry_t* ent) {
    pipe_object_t* pipe;

    if (!ent || !ent->valid || ent->kind != PROCESS_HANDLE_KIND_PIPE) return;
    pipe = pipe_object_from_ent(ent);
    if (!pipe) return;
    if (ent->readable) pipe->read_refs++;
    if (ent->writable) pipe->write_refs++;
}

static void process_fd_pty_ref(fd_entry_t* ent) {
    pty_object_t* pty;

    if (!ent || !ent->valid) return;
    pty = pty_object_from_ent(ent);
    if (!pty) return;
    if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER) pty->master_refs++;
    if (ent->kind == PROCESS_HANDLE_KIND_PTY_SLAVE) pty->slave_refs++;
}

static void process_fd_share_ref(fd_entry_t* ent) {
    if (!ent || !ent->valid) return;
    if (ent->kind == PROCESS_HANDLE_KIND_PIPE) {
        process_fd_pipe_ref(ent);
    } else if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER ||
               ent->kind == PROCESS_HANDLE_KIND_PTY_SLAVE) {
        process_fd_pty_ref(ent);
    } else if (ent->kind == PROCESS_HANDLE_KIND_SOCKET) {
        socket_retain(ent->socket);
    } else if (ent->kind == PROCESS_HANDLE_KIND_FILE) {
        vfs_file_retain(ent);
    } else if (ent->kind == PROCESS_HANDLE_KIND_VIRTUAL) {
        virtual_object_t* obj = virtual_object_from_ent(ent);
        if (obj) obj->refs++;
    } else if (ent->kind == PROCESS_HANDLE_KIND_EPOLL ||
               ent->kind == PROCESS_HANDLE_KIND_TIMERFD ||
               ent->kind == PROCESS_HANDLE_KIND_SIGNALFD) {
        ent->aux_frame = 0;
    }
}

int process_fd_pipe(process_t* proc, int fds[2], unsigned int flags) {
    fd_entry_t* read_ent;
    fd_entry_t* write_ent;
    pipe_object_t* pipe;
    u32 pipe_frame;
    u32 data_frame;
    int read_fd;
    int write_fd;

    if (!proc || !fds) return -EINVAL;
    if ((flags & ~(SYS_FD_FLAG_NONBLOCK | SYS_FD_FLAG_CLOEXEC)) != 0u) {
        return -EINVAL;
    }

    pipe_frame = pmm_alloc_frame();
    if (!pipe_frame) return -ENOMEM;
    data_frame = pmm_alloc_frame();
    if (!data_frame) {
        pmm_free_frame(pipe_frame);
        return -ENOMEM;
    }

    pipe = (pipe_object_t*)paging_phys_to_kernel_virt(pipe_frame);
    k_memset(pipe, 0, PAGE_SIZE);
    k_memset(paging_phys_to_kernel_virt(data_frame), 0, PAGE_SIZE);
    wait_queue_init(&pipe->read_waiters);
    wait_queue_init(&pipe->write_waiters);
    pipe->data_frame = data_frame;
    pipe->read_refs = 1;
    pipe->write_refs = 1;

    read_fd = process_fd_alloc_entry(proc, &read_ent);
    if (read_fd < 0) {
        pmm_free_frame(data_frame);
        pmm_free_frame(pipe_frame);
        return read_fd;
    }
    read_ent->valid = 1;

    write_fd = process_fd_alloc_entry(proc, &write_ent);
    if (write_fd < 0) {
        k_memset(read_ent, 0, sizeof(*read_ent));
        pmm_free_frame(data_frame);
        pmm_free_frame(pipe_frame);
        return write_fd;
    }

    k_memset(read_ent, 0, sizeof(*read_ent));
    read_ent->valid = 1;
    read_ent->kind = PROCESS_HANDLE_KIND_PIPE;
    read_ent->ops = &s_pipe_handle_ops;
    read_ent->readable = 1;
    read_ent->writable = 0;
    read_ent->flags = flags & SYS_FD_FLAG_NONBLOCK;
    read_ent->fd_flags = flags & SYS_FD_FLAG_CLOEXEC;
    read_ent->aux_frame = pipe_frame;
    k_memcpy(read_ent->name, "pipe:r", 7);

    k_memset(write_ent, 0, sizeof(*write_ent));
    write_ent->valid = 1;
    write_ent->kind = PROCESS_HANDLE_KIND_PIPE;
    write_ent->ops = &s_pipe_handle_ops;
    write_ent->readable = 0;
    write_ent->writable = 1;
    write_ent->flags = flags & SYS_FD_FLAG_NONBLOCK;
    write_ent->fd_flags = flags & SYS_FD_FLAG_CLOEXEC;
    write_ent->aux_frame = pipe_frame;
    k_memcpy(write_ent->name, "pipe:w", 7);

    fds[0] = read_fd;
    fds[1] = write_fd;
    return 0;
}

int process_fd_pty(process_t* proc, int fds[2], unsigned int master_flags) {
    fd_entry_t* master_ent;
    fd_entry_t* slave_ent;
    pty_object_t* pty;
    u32 pty_frame;
    u32 m2s_frame;
    u32 s2m_frame;
    int master_fd;
    int slave_fd;

    if (!proc || !fds) return -EINVAL;
    if ((master_flags & ~(SYS_FD_FLAG_NONBLOCK | SYS_FD_FLAG_CLOEXEC)) != 0u) {
        return -EINVAL;
    }

    pty_frame = pmm_alloc_frame();
    if (!pty_frame) return -ENOMEM;
    m2s_frame = pmm_alloc_frame();
    if (!m2s_frame) {
        pmm_free_frame(pty_frame);
        return -ENOMEM;
    }
    s2m_frame = pmm_alloc_frame();
    if (!s2m_frame) {
        pmm_free_frame(m2s_frame);
        pmm_free_frame(pty_frame);
        return -ENOMEM;
    }

    pty = (pty_object_t*)paging_phys_to_kernel_virt(pty_frame);
    k_memset(pty, 0, PAGE_SIZE);
    k_memset(paging_phys_to_kernel_virt(m2s_frame), 0, PAGE_SIZE);
    k_memset(paging_phys_to_kernel_virt(s2m_frame), 0, PAGE_SIZE);
    wait_queue_init(&pty->master_to_slave.read_waiters);
    wait_queue_init(&pty->master_to_slave.write_waiters);
    wait_queue_init(&pty->slave_to_master.read_waiters);
    wait_queue_init(&pty->slave_to_master.write_waiters);
    pty->master_to_slave.data_frame = m2s_frame;
    pty->slave_to_master.data_frame = s2m_frame;
    pty->master_refs = 1;
    pty->slave_refs = 1;
    pty->rows = 25;
    pty->cols = 80;
    terminal_attr_init_default(&pty->termios);

    master_fd = process_fd_alloc_entry(proc, &master_ent);
    if (master_fd < 0) {
        pmm_free_frame(s2m_frame);
        pmm_free_frame(m2s_frame);
        pmm_free_frame(pty_frame);
        return master_fd;
    }
    master_ent->valid = 1;

    slave_fd = process_fd_alloc_entry(proc, &slave_ent);
    if (slave_fd < 0) {
        k_memset(master_ent, 0, sizeof(*master_ent));
        pmm_free_frame(s2m_frame);
        pmm_free_frame(m2s_frame);
        pmm_free_frame(pty_frame);
        return slave_fd;
    }

    k_memset(master_ent, 0, sizeof(*master_ent));
    master_ent->valid = 1;
    master_ent->kind = PROCESS_HANDLE_KIND_PTY_MASTER;
    master_ent->ops = &s_pty_handle_ops;
    master_ent->readable = 1;
    master_ent->writable = 1;
    master_ent->flags = master_flags & SYS_FD_FLAG_NONBLOCK;
    master_ent->fd_flags = master_flags & SYS_FD_FLAG_CLOEXEC;
    master_ent->aux_frame = pty_frame;
    k_memcpy(master_ent->name, "pty:master", 11);

    k_memset(slave_ent, 0, sizeof(*slave_ent));
    slave_ent->valid = 1;
    slave_ent->kind = PROCESS_HANDLE_KIND_PTY_SLAVE;
    slave_ent->ops = &s_pty_handle_ops;
    slave_ent->readable = 1;
    slave_ent->writable = 1;
    slave_ent->aux_frame = pty_frame;
    k_memcpy(slave_ent->name, "pty:slave", 10);

    fds[0] = master_fd;
    fds[1] = slave_fd;
    return 0;
}

int process_fd_pty_set_size(fd_entry_t* ent, unsigned int rows, unsigned int cols) {
    pty_object_t* pty = pty_object_from_ent(ent);
    if (!pty) return -ENOTTY;
    if (rows == 0u || cols == 0u || rows > 200u || cols > 240u) return -EINVAL;
    pty->rows = rows;
    pty->cols = cols;
    return 0;
}

int process_fd_terminal_size(fd_entry_t* ent, unsigned int* out_rows, unsigned int* out_cols) {
    pty_object_t* pty = pty_object_from_ent(ent);
    if (!out_rows || !out_cols) return -EFAULT;
    if (pty) {
        *out_rows = pty->rows ? pty->rows : 25u;
        *out_cols = pty->cols ? pty->cols : 80u;
        return 0;
    }
    if (!ent || !ent->valid) return -ENOTTY;
    if (ent->kind == PROCESS_HANDLE_KIND_CONSOLE || virtual_entry_is_tty(ent)) {
        *out_rows = (unsigned int)terminal_rows();
        *out_cols = (unsigned int)terminal_cols();
        return 0;
    }
    return -ENOTTY;
}

int process_fd_is_terminal(fd_entry_t* ent) {
    if (!ent || !ent->valid) return 0;
    if (ent->kind == PROCESS_HANDLE_KIND_CONSOLE) return 1;
    if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER ||
        ent->kind == PROCESS_HANDLE_KIND_PTY_SLAVE) return 1;
    return virtual_entry_is_tty(ent);
}

int process_fd_terminal_getattr(fd_entry_t* ent, sys_termios_t* out) {
    pty_object_t* pty;
    if (!out) return -EFAULT;
    if (!process_fd_is_terminal(ent)) return -ENOTTY;
    pty = pty_object_from_ent(ent);
    if (pty) {
        k_memcpy(out, &pty->termios, sizeof(*out));
    } else {
        k_memcpy(out, console_termios(), sizeof(*out));
    }
    return 0;
}

int process_fd_terminal_setattr(fd_entry_t* ent, const sys_termios_t* in) {
    pty_object_t* pty;
    if (!in) return -EFAULT;
    if (!process_fd_is_terminal(ent)) return -ENOTTY;
    pty = pty_object_from_ent(ent);
    if (pty) {
        k_memcpy(&pty->termios, in, sizeof(*in));
    } else {
        k_memcpy(console_termios(), in, sizeof(*in));
    }
    return 0;
}

int process_fd_pty_set_foreground(fd_entry_t* ent, u32 pgid) {
    pty_object_t* pty = pty_object_from_ent(ent);
    if (!pty) return -ENOTTY;
    pty->foreground_pgid = pgid;
    return 0;
}

u32 process_fd_pty_get_foreground(fd_entry_t* ent) {
    pty_object_t* pty = pty_object_from_ent(ent);
    return pty ? pty->foreground_pgid : 0u;
}

int process_fd_terminal_get_pgrp(fd_entry_t* ent, u32 fallback_pgid, u32* out_pgid) {
    pty_object_t* pty;
    if (!out_pgid) return -EFAULT;
    if (!process_fd_is_terminal(ent)) return -ENOTTY;
    pty = pty_object_from_ent(ent);
    *out_pgid = pty && pty->foreground_pgid ? pty->foreground_pgid : fallback_pgid;
    return 0;
}

int process_fd_terminal_set_pgrp(fd_entry_t* ent, u32 pgid) {
    pty_object_t* pty;
    if (pgid == 0u) return -EINVAL;
    if (!process_fd_is_terminal(ent)) return -ENOTTY;
    pty = pty_object_from_ent(ent);
    if (pty) {
        pty->foreground_pgid = pgid;
    } else {
        s_foreground_pgid = pgid;
    }
    return 0;
}

int process_fd_dup(process_t* proc, int oldfd, int minfd, unsigned int fd_flags) {
    fd_entry_t* old_ent;
    fd_entry_t* new_ent;
    int new_fd;

    if (!proc) return -EINVAL;
    if (minfd < 0 || (unsigned int)minfd >= proc->fd_limit) return -EBADF;
    old_ent = process_fd_get(proc, oldfd);
    if (!old_ent) return -EBADF;
    if ((fd_flags & ~SYS_FD_FLAG_CLOEXEC) != 0u) return -EINVAL;

    for (;;) {
        while ((unsigned int)minfd >= proc->fd_capacity) {
            int rc = process_fd_table_grow(proc, (unsigned int)minfd + 1u);
            if (rc < 0) return rc;
            old_ent = process_fd_get(proc, oldfd);
            if (!old_ent) return -EBADF;
        }
        for (unsigned int fd = (unsigned int)minfd; fd < proc->fd_capacity; fd++) {
            if (!proc->fds[fd].valid) {
                new_fd = (int)fd;
                new_ent = &proc->fds[fd];
                *new_ent = *old_ent;
                new_ent->fd_flags = fd_flags;
                process_fd_share_ref(new_ent);
                return new_fd;
            }
        }
        if (proc->fd_capacity >= proc->fd_limit) return -ENFILE;
        minfd = (int)proc->fd_capacity;
    }
}

int process_fd_dup2(process_t* proc, int oldfd, int newfd, unsigned int fd_flags, int reject_same) {
    fd_entry_t* old_ent;
    fd_entry_t* new_ent;
    int rc;

    if (!proc) return -EINVAL;
    if ((fd_flags & ~SYS_FD_FLAG_CLOEXEC) != 0u) return -EINVAL;
    old_ent = process_fd_get(proc, oldfd);
    if (!old_ent) return -EBADF;
    if (newfd < 0 || (unsigned int)newfd >= proc->fd_limit) return -EBADF;
    if (oldfd == newfd) {
        return reject_same ? -EINVAL : newfd;
    }

    rc = process_fd_alloc_exact(proc, newfd, &new_ent);
    if (rc < 0) return rc;
    old_ent = process_fd_get(proc, oldfd);
    if (!old_ent) return -EBADF;
    *new_ent = *old_ent;
    new_ent->fd_flags = fd_flags;
    process_fd_share_ref(new_ent);
    return newfd;
}

int process_fd_dup_from(process_t* dst, int newfd, process_t* src, int oldfd, unsigned int fd_flags) {
    fd_entry_t* old_ent;
    fd_entry_t* new_ent;
    int rc;

    if (!dst || !src) return -EINVAL;
    if ((fd_flags & ~SYS_FD_FLAG_CLOEXEC) != 0u) return -EINVAL;
    old_ent = process_fd_get(src, oldfd);
    if (!old_ent) return -EBADF;
    if (newfd < 0 || (unsigned int)newfd >= dst->fd_limit) return -EBADF;

    rc = process_fd_alloc_exact(dst, newfd, &new_ent);
    if (rc < 0) return rc;
    if (dst == src) {
        old_ent = process_fd_get(src, oldfd);
        if (!old_ent) return -EBADF;
    }
    *new_ent = *old_ent;
    new_ent->fd_flags = fd_flags;
    process_fd_share_ref(new_ent);
    return newfd;
}

void process_fd_close(fd_entry_t* ent) {
    if (!ent) return;

    if (ent->writable) {
        (void)process_fd_flush(ent);
    }

    if (ent->ops && ent->ops->close) {
        ent->ops->close(ent);
        return;
    }

    k_memset(ent, 0, sizeof(*ent));
}

static int process_handle_socket_read(fd_entry_t* ent, char* buf, unsigned int len) {
    int rc;
    socket_t* sock;
    process_t* proc;

    if (!ent || !ent->valid) return -EBADF;
    sock = ent->socket;
    if (socket_state(sock) != SOCKET_STATE_CONNECTED &&
        socket_state(sock) != SOCKET_STATE_CONNECTING) return -EINVAL;
    if (len == 0) return 0;

    if (!socket_tcp_recv_ready(sock)) {
        if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            return -EAGAIN;
        }
    }

    proc = sched_current();
    while (!socket_tcp_recv_ready(sock)) {
        int wait_rc;

        if (!socket_tcp_connection_established(sock)) {
            if (!socket_tcp_connect_pending(sock)) {
                __asm__ __volatile__("cli");
                return 0;
            }

            if (!proc) {
                __asm__ __volatile__("sti; hlt; cli");
                continue;
            }

            proc->state = PROCESS_STATE_WAITING;
            wait_rc = socket_wait(sock, proc, POLLOUT);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                socket_wait_clear_process(proc);
                return wait_rc;
            }
            __asm__ __volatile__("sti");
            while (proc->state != PROCESS_STATE_RUNNING) {
                __asm__ __volatile__("hlt");
            }
            __asm__ __volatile__("cli");
            socket_wait_clear_process(proc);
            continue;
        }

        if (!proc) {
            __asm__ __volatile__("sti; hlt; cli");
            continue;
        }

        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(sock, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_tcp_recv_ready(sock)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }

        __asm__ __volatile__("sti");
        while (proc->state != PROCESS_STATE_RUNNING) {
            __asm__ __volatile__("hlt");
        }
        __asm__ __volatile__("cli");
        socket_wait_clear_process(proc);
    }
    __asm__ __volatile__("cli");
    socket_wait_clear_process(proc);
    rc = socket_tcp_recv(sock, buf, len);
    return rc < 0 ? -ECONNRESET : rc;
}

static int process_handle_socket_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    int rc;
    socket_t* sock;
    process_t* proc;
    unsigned int done = 0u;

    if (!ent || !ent->valid) return -EBADF;
    sock = ent->socket;
    if (socket_state(sock) != SOCKET_STATE_CONNECTED &&
        socket_state(sock) != SOCKET_STATE_CONNECTING) return -EINVAL;
    if (len == 0) return 0;

    proc = sched_current();
    while (done < len) {
        rc = socket_tcp_send(sock, buf + done, len - done);
        if (rc > 0) {
            done += (unsigned int)rc;
            if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
                break;
            }
            continue;
        }

        if (rc < 0 && rc != -EAGAIN) {
            if (done) {
                return (int)done;
            }
            if (rc == -ENOMEM || rc == -EPIPE) {
                if (rc == -EPIPE && proc) {
                    (void)process_signal_deliver(proc, PROCESS_SIGPIPE);
                }
                return rc;
            }
            return -ECONNRESET;
        }

        if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            return done ? (int)done : -EAGAIN;
        }

        if (!socket_tcp_connection_established(sock)) {
            if (!socket_tcp_connect_pending(sock)) {
                return done ? (int)done : -ECONNRESET;
            }
        }

        if (!proc) {
            __asm__ __volatile__("sti; hlt; cli");
            continue;
        }

        proc->state = PROCESS_STATE_WAITING;
        rc = socket_wait(sock, proc, POLLOUT);
        if (rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return done ? (int)done : rc;
        }
        if (socket_tcp_send_ready(sock) ||
            !socket_tcp_connection_established(sock)) {
            proc->state = PROCESS_STATE_RUNNING;
        }

        __asm__ __volatile__("sti");
        while (proc->state != PROCESS_STATE_RUNNING) {
            __asm__ __volatile__("hlt");
        }
        __asm__ __volatile__("cli");
        socket_wait_clear_process(proc);
    }

    return (int)done;
}

static int process_handle_socket_seek(fd_entry_t* ent, int offset, int whence) {
    (void)ent;
    (void)offset;
    (void)whence;
    return -ENOSYS;
}

static short process_handle_socket_poll(fd_entry_t* ent, short events) {
    if (!ent || !ent->valid) return POLLERR;
    return socket_poll(ent->socket, events);
}

static int process_handle_console_read_common(fd_entry_t* ent, char* buf, unsigned int len, int echo) {
    process_t* proc = sched_current();
    sys_termios_t* tio = console_termios();
    int canonical = echo && ((tio->c_lflag & TERM_LFLAG_ICANON) != 0u);
    int do_echo = echo && ((tio->c_lflag & TERM_LFLAG_ECHO) != 0u);
    unsigned int n = 0;

    if (!ent || !ent->valid || !ent->readable) return -EBADF;
    if (!buf) return -EFAULT;
    if (len == 0) return 0;

    while (n < len) {
        for (;;) {
            __asm__ volatile ("cli");
            if (keyboard_buf_available()) {
                break;
            }
            if (proc) {
                proc->state = PROCESS_STATE_WAITING;
                keyboard_set_waiting_process(proc);
                if (!echo) s_raw_console_reader = proc;
            }
            __asm__ volatile ("sti; hlt");
        }

        char c = keyboard_buf_pop();
        if (s_raw_console_reader == proc) s_raw_console_reader = 0;
        if (canonical && (unsigned char)c == tio->c_cc[TERM_VEOF]) {
            if (n == 0u) {
                __asm__ volatile ("cli");
                return 0;
            }
            break;
        }
        if (do_echo) {
            terminal_putc(c);
        }
        buf[n++] = c;
        if (canonical) {
            if (c == '\n') break;
        } else {
            break;
        }
    }

    __asm__ volatile ("cli");
    return (int)n;
}

static int process_handle_console_read(fd_entry_t* ent, char* buf, unsigned int len) {
    return process_handle_console_read_common(ent, buf, len, 1);
}

static int process_handle_console_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    if (!ent || !ent->valid || !ent->writable) return -EBADF;
    if (!buf) return -EFAULT;

    terminal_write(buf, len);
    return (int)len;
}

static int process_handle_console_seek(fd_entry_t* ent, int offset, int whence) {
    (void)ent;
    (void)offset;
    (void)whence;
    return -ENOSYS;
}

static short process_handle_console_poll(fd_entry_t* ent, short events) {
    short revents = 0;

    if (!ent || !ent->valid) return POLLERR;
    if (ent->readable && (events & POLLIN) && keyboard_buf_available()) {
        revents |= POLLIN;
    }
    if (ent->writable && (events & POLLOUT)) {
        revents |= POLLOUT;
    }
    return revents;
}

static void process_handle_console_close(fd_entry_t* ent) {
    if (!ent) return;
    k_memset(ent, 0, sizeof(*ent));
}

static int process_handle_pipe_read(fd_entry_t* ent, char* buf, unsigned int len) {
    pipe_object_t* pipe;
    char* data;
    process_t* proc;
    unsigned int done = 0u;

    if (!ent || !ent->valid || !ent->readable) return -EBADF;
    if (!buf) return -EFAULT;
    if (len == 0) return 0;
    pipe = pipe_object_from_ent(ent);
    data = pipe_data(pipe);
    if (!pipe || !data) return -EIO;

    proc = sched_current();
    while (pipe->count == 0u && pipe->write_refs != 0u) {
        int wait_rc;
        if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            return -EAGAIN;
        }
        if (!proc) {
            __asm__ __volatile__("sti; hlt; cli");
            continue;
        }
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = wait_queue_add(&pipe->read_waiters, proc);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            wait_queue_remove_proc(proc);
            return wait_rc;
        }
        if (pipe->count != 0u || pipe->write_refs == 0u) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        __asm__ __volatile__("sti");
        while (proc->state != PROCESS_STATE_RUNNING) {
            __asm__ __volatile__("hlt");
        }
        __asm__ __volatile__("cli");
        wait_queue_remove_proc(proc);
    }
    wait_queue_remove_proc(proc);

    while (done < len && pipe->count != 0u) {
        buf[done++] = data[pipe->read_pos];
        pipe->read_pos = (pipe->read_pos + 1u) % PIPE_BUFFER_SIZE;
        pipe->count--;
    }

    if (done != 0u) {
        wait_queue_wake_all(&pipe->write_waiters);
    }
    return (int)done;
}

static int process_handle_pipe_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    pipe_object_t* pipe;
    char* data;
    process_t* proc;
    unsigned int done = 0u;
    unsigned int atomic_len;

    if (!ent || !ent->valid || !ent->writable) return -EBADF;
    if (!buf) return -EFAULT;
    if (len == 0) return 0;
    pipe = pipe_object_from_ent(ent);
    data = pipe_data(pipe);
    if (!pipe || !data) return -EIO;

    proc = sched_current();
    atomic_len = (len <= SYS_PIPE_BUF) ? len : 0u;
    while (done < len) {
        unsigned int needed = atomic_len ? atomic_len : 1u;
        if (pipe->read_refs == 0u) {
            if (done != 0u) return (int)done;
            if (proc) (void)process_signal_deliver(proc, PROCESS_SIGPIPE);
            return -EPIPE;
        }

        while ((PIPE_BUFFER_SIZE - pipe->count) < needed && pipe->read_refs != 0u) {
            int wait_rc;
            if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
                return done ? (int)done : -EAGAIN;
            }
            if (!proc) {
                __asm__ __volatile__("sti; hlt; cli");
                continue;
            }
            proc->state = PROCESS_STATE_WAITING;
            wait_rc = wait_queue_add(&pipe->write_waiters, proc);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                wait_queue_remove_proc(proc);
                return done ? (int)done : wait_rc;
            }
            if ((PIPE_BUFFER_SIZE - pipe->count) >= needed || pipe->read_refs == 0u) {
                proc->state = PROCESS_STATE_RUNNING;
                break;
            }
            __asm__ __volatile__("sti");
            while (proc->state != PROCESS_STATE_RUNNING) {
                __asm__ __volatile__("hlt");
            }
            __asm__ __volatile__("cli");
            wait_queue_remove_proc(proc);
        }
        wait_queue_remove_proc(proc);

        if (pipe->read_refs == 0u) {
            if (done != 0u) return (int)done;
            if (proc) (void)process_signal_deliver(proc, PROCESS_SIGPIPE);
            return -EPIPE;
        }

        while (done < len && pipe->count < PIPE_BUFFER_SIZE) {
            data[pipe->write_pos] = buf[done++];
            pipe->write_pos = (pipe->write_pos + 1u) % PIPE_BUFFER_SIZE;
            pipe->count++;
        }
        wait_queue_wake_all(&pipe->read_waiters);

        if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            break;
        }
    }

    return (int)done;
}

static int process_handle_pipe_seek(fd_entry_t* ent, int offset, int whence) {
    (void)ent;
    (void)offset;
    (void)whence;
    return -ESPIPE;
}

static short process_handle_pipe_poll(fd_entry_t* ent, short events) {
    pipe_object_t* pipe;
    short revents = 0;

    if (!ent || !ent->valid) return POLLERR;
    pipe = pipe_object_from_ent(ent);
    if (!pipe) return POLLERR;

    if ((events & POLLIN) && ent->readable &&
        (pipe->count != 0u || pipe->write_refs == 0u)) {
        revents |= POLLIN;
    }
    if ((events & POLLOUT) && ent->writable &&
        pipe->read_refs != 0u && pipe->count < PIPE_BUFFER_SIZE) {
        revents |= POLLOUT;
    }
    if ((ent->readable && pipe->write_refs == 0u) ||
        (ent->writable && pipe->read_refs == 0u)) {
        revents |= POLLHUP;
    }
    return revents;
}

static void process_handle_pipe_close(fd_entry_t* ent) {
    pipe_object_t* pipe;
    u32 pipe_frame;
    u32 data_frame;

    if (!ent) return;
    pipe = pipe_object_from_ent(ent);
    pipe_frame = ent->aux_frame;
    if (!pipe) {
        k_memset(ent, 0, sizeof(*ent));
        return;
    }

    if (ent->readable && pipe->read_refs > 0u) pipe->read_refs--;
    if (ent->writable && pipe->write_refs > 0u) pipe->write_refs--;
    wait_queue_wake_all(&pipe->read_waiters);
    wait_queue_wake_all(&pipe->write_waiters);

    data_frame = pipe->data_frame;
    if (pipe->read_refs == 0u && pipe->write_refs == 0u) {
        if (data_frame) pmm_free_frame(data_frame);
        if (pipe_frame) pmm_free_frame(pipe_frame);
    }
    k_memset(ent, 0, sizeof(*ent));
}

static int pty_buffer_read(pty_buffer_t* buffer,
                           char* buf,
                           unsigned int len,
                           unsigned int* writer_refs,
                           unsigned int flags) {
    char* data;
    process_t* proc;
    unsigned int done = 0u;

    if (!buffer || !buf) return -EFAULT;
    if (len == 0) return 0;
    data = pty_buffer_data(buffer);
    if (!data) return -EIO;

    proc = sched_current();
    while (buffer->count == 0u && (!writer_refs || *writer_refs != 0u)) {
        int wait_rc;
        if ((flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            return -EAGAIN;
        }
        if (!proc) {
            __asm__ __volatile__("sti; hlt; cli");
            continue;
        }
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = wait_queue_add(&buffer->read_waiters, proc);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            wait_queue_remove_proc(proc);
            return wait_rc;
        }
        if (buffer->count != 0u || (writer_refs && *writer_refs == 0u)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        __asm__ __volatile__("sti");
        while (proc->state != PROCESS_STATE_RUNNING) {
            __asm__ __volatile__("hlt");
        }
        __asm__ __volatile__("cli");
        wait_queue_remove_proc(proc);
    }
    wait_queue_remove_proc(proc);

    while (done < len && buffer->count != 0u) {
        buf[done++] = data[buffer->read_pos];
        buffer->read_pos = (buffer->read_pos + 1u) % PIPE_BUFFER_SIZE;
        buffer->count--;
    }
    if (done != 0u) wait_queue_wake_all(&buffer->write_waiters);
    return (int)done;
}

static int pty_buffer_write(pty_buffer_t* buffer,
                            const char* buf,
                            unsigned int len,
                            unsigned int* reader_refs,
                            unsigned int flags) {
    char* data;
    process_t* proc;
    unsigned int done = 0u;

    if (!buffer || !buf) return -EFAULT;
    if (len == 0) return 0;
    data = pty_buffer_data(buffer);
    if (!data) return -EIO;

    proc = sched_current();
    while (done < len) {
        if (reader_refs && *reader_refs == 0u) {
            return done ? (int)done : -EIO;
        }

        while (buffer->count >= PIPE_BUFFER_SIZE && (!reader_refs || *reader_refs != 0u)) {
            int wait_rc;
            if ((flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
                return done ? (int)done : -EAGAIN;
            }
            if (!proc) {
                __asm__ __volatile__("sti; hlt; cli");
                continue;
            }
            proc->state = PROCESS_STATE_WAITING;
            wait_rc = wait_queue_add(&buffer->write_waiters, proc);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                wait_queue_remove_proc(proc);
                return done ? (int)done : wait_rc;
            }
            if (buffer->count < PIPE_BUFFER_SIZE || (reader_refs && *reader_refs == 0u)) {
                proc->state = PROCESS_STATE_RUNNING;
                break;
            }
            __asm__ __volatile__("sti");
            while (proc->state != PROCESS_STATE_RUNNING) {
                __asm__ __volatile__("hlt");
            }
            __asm__ __volatile__("cli");
            wait_queue_remove_proc(proc);
        }
        wait_queue_remove_proc(proc);

        if (reader_refs && *reader_refs == 0u) return done ? (int)done : -EIO;

        while (done < len && buffer->count < PIPE_BUFFER_SIZE) {
            data[buffer->write_pos] = buf[done++];
            buffer->write_pos = (buffer->write_pos + 1u) % PIPE_BUFFER_SIZE;
            buffer->count++;
        }
        wait_queue_wake_all(&buffer->read_waiters);

        if ((flags & SYS_FD_FLAG_NONBLOCK) != 0u) break;
    }
    return (int)done;
}

static int pty_buffer_write_output(pty_buffer_t* buffer,
                                   const char* buf,
                                   unsigned int len,
                                   unsigned int* reader_refs,
                                   unsigned int flags,
                                   const sys_termios_t* tio,
                                   unsigned int* prev_cr) {
    unsigned int done = 0u;
    int translate_newline;

    if (!buf) return -EFAULT;
    translate_newline = tio &&
                        ((tio->c_oflag & TERM_OFLAG_OPOST) != 0u) &&
                        ((tio->c_oflag & TERM_OFLAG_ONLCR) != 0u);
    if (!translate_newline) {
        return pty_buffer_write(buffer, buf, len, reader_refs, flags);
    }

    while (done < len) {
        int rc;
        if (buf[done] == '\n') {
            if (prev_cr && *prev_cr) {
                rc = pty_buffer_write(buffer, &buf[done], 1u, reader_refs, flags);
            } else {
                char crlf[2] = { '\r', '\n' };
                rc = pty_buffer_write(buffer, crlf, sizeof(crlf), reader_refs, flags);
            }
        } else {
            rc = pty_buffer_write(buffer, &buf[done], 1u, reader_refs, flags);
        }
        if (rc < 0) return done ? (int)done : rc;
        if (rc == 0) return (int)done;
        if (prev_cr) *prev_cr = (buf[done] == '\r') ? 1u : 0u;
        done++;
        if ((flags & SYS_FD_FLAG_NONBLOCK) != 0u) break;
    }
    return (int)done;
}

static int process_handle_pty_read_common(fd_entry_t* ent,
                                          char* buf,
                                          unsigned int len,
                                          int echo) {
    pty_object_t* pty;
    pty_buffer_t* in;
    pty_buffer_t* out;
    unsigned int done = 0u;

    if (!ent || !ent->valid || !ent->readable) return -EBADF;
    if (!buf) return -EFAULT;
    if (len == 0) return 0;
    pty = pty_object_from_ent(ent);
    if (!pty) return -EIO;

    if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER) {
        return pty_buffer_read(&pty->slave_to_master, buf, len,
                               &pty->slave_refs, ent->flags);
    }

    in = &pty->master_to_slave;
    out = &pty->slave_to_master;
    while (done < len) {
        int rc = pty_buffer_read(in, &buf[done], 1u, &pty->master_refs, ent->flags);
        int canonical = echo && ((pty->termios.c_lflag & TERM_LFLAG_ICANON) != 0u);
        int do_echo = echo && ((pty->termios.c_lflag & TERM_LFLAG_ECHO) != 0u);
        if (rc <= 0) return done ? (int)done : rc;
        if (canonical && (unsigned char)buf[done] == pty->termios.c_cc[TERM_VEOF]) {
            if (done == 0u) return 0;
            break;
        }
        if (do_echo) {
            (void)pty_buffer_write_output(out,
                                          &buf[done],
                                          1u,
                                          &pty->master_refs,
                                          0,
                                          &pty->termios,
                                          &pty->output_prev_cr);
        }
        done++;
        if (canonical) {
            if (buf[done - 1] == '\n') break;
        } else {
            break;
        }
    }
    return (int)done;
}

static int process_handle_pty_read(fd_entry_t* ent, char* buf, unsigned int len) {
    return process_handle_pty_read_common(ent, buf, len, 1);
}

static int process_handle_pty_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    pty_object_t* pty;

    if (!ent || !ent->valid || !ent->writable) return -EBADF;
    if (!buf) return -EFAULT;
    if (len == 0) return 0;
    pty = pty_object_from_ent(ent);
    if (!pty) return -EIO;

    if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER) {
        for (unsigned int i = 0; i < len; i++) {
            u32 interrupt_pgid = pty->foreground_pgid
                               ? pty->foreground_pgid
                               : s_foreground_pgid;
            unsigned char vintr = pty->termios.c_cc[TERM_VINTR]
                                ? pty->termios.c_cc[TERM_VINTR]
                                : 3u;
            unsigned char vsusp = pty->termios.c_cc[TERM_VSUSP]
                                ? pty->termios.c_cc[TERM_VSUSP]
                                : 26u;
            if ((pty->termios.c_lflag & TERM_LFLAG_ISIG) != 0u &&
                (unsigned char)buf[i] == vintr &&
                interrupt_pgid != 0u) {
                char interrupt_text[] = "^C\n";
                if (!process_group_signal_deliver(interrupt_pgid, PROCESS_SIGINT)) {
                    (void)process_group_kill(interrupt_pgid,
                                             PROCESS_TERMINATED_BY_CTRL_C);
                }
                if ((pty->termios.c_lflag & TERM_LFLAG_ECHO) != 0u) {
                    (void)pty_buffer_write_output(&pty->slave_to_master,
                                                  interrupt_text,
                                                  sizeof(interrupt_text) - 1u,
                                                  &pty->master_refs,
                                                  0,
                                                  &pty->termios,
                                                  &pty->output_prev_cr);
                }
                if (i > 0) {
                    return (int)i;
                }
                return 1;
            }
            if ((pty->termios.c_lflag & TERM_LFLAG_ISIG) != 0u &&
                (unsigned char)buf[i] == vsusp &&
                interrupt_pgid != 0u) {
                char stop_text[] = "^Z\n";
                (void)process_group_stop(interrupt_pgid,
                                         PROCESS_SIGTSTP,
                                         sched_current(),
                                         0);
                if ((pty->termios.c_lflag & TERM_LFLAG_ECHO) != 0u) {
                    (void)pty_buffer_write_output(&pty->slave_to_master,
                                                  stop_text,
                                                  sizeof(stop_text) - 1u,
                                                  &pty->master_refs,
                                                  0,
                                                  &pty->termios,
                                                  &pty->output_prev_cr);
                }
                if (i > 0) {
                    return (int)i;
                }
                return 1;
            }
        }
        return pty_buffer_write(&pty->master_to_slave, buf, len,
                                &pty->slave_refs, ent->flags);
    }
    return pty_buffer_write_output(&pty->slave_to_master,
                                   buf,
                                   len,
                                   &pty->master_refs,
                                   ent->flags,
                                   &pty->termios,
                                   &pty->output_prev_cr);
}

static int process_handle_pty_seek(fd_entry_t* ent, int offset, int whence) {
    (void)ent;
    (void)offset;
    (void)whence;
    return -EINVAL;
}

static short process_handle_pty_poll(fd_entry_t* ent, short events) {
    pty_object_t* pty;
    pty_buffer_t* read_buf;
    pty_buffer_t* write_buf;
    unsigned int peer_refs;
    short revents = 0;

    if (!ent || !ent->valid) return POLLERR;
    pty = pty_object_from_ent(ent);
    if (!pty) return POLLERR;

    if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER) {
        read_buf = &pty->slave_to_master;
        write_buf = &pty->master_to_slave;
        peer_refs = pty->slave_refs;
    } else {
        read_buf = &pty->master_to_slave;
        write_buf = &pty->slave_to_master;
        peer_refs = pty->master_refs;
    }

    if ((events & POLLIN) && ent->readable &&
        (read_buf->count != 0u || peer_refs == 0u)) {
        revents |= POLLIN;
    }
    if ((events & POLLOUT) && ent->writable &&
        peer_refs != 0u && write_buf->count < PIPE_BUFFER_SIZE) {
        revents |= POLLOUT;
    }
    if (peer_refs == 0u) revents |= POLLHUP;
    return revents;
}

static void process_handle_pty_close(fd_entry_t* ent) {
    pty_object_t* pty;
    u32 pty_frame;
    u32 m2s_frame;
    u32 s2m_frame;

    if (!ent) return;
    pty = pty_object_from_ent(ent);
    pty_frame = ent->aux_frame;
    if (!pty) {
        k_memset(ent, 0, sizeof(*ent));
        return;
    }

    if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER && pty->master_refs > 0u) {
        pty->master_refs--;
    }
    if (ent->kind == PROCESS_HANDLE_KIND_PTY_SLAVE && pty->slave_refs > 0u) {
        pty->slave_refs--;
    }
    wait_queue_wake_all(&pty->master_to_slave.read_waiters);
    wait_queue_wake_all(&pty->master_to_slave.write_waiters);
    wait_queue_wake_all(&pty->slave_to_master.read_waiters);
    wait_queue_wake_all(&pty->slave_to_master.write_waiters);

    m2s_frame = pty->master_to_slave.data_frame;
    s2m_frame = pty->slave_to_master.data_frame;
    if (pty->master_refs == 0u && pty->slave_refs == 0u) {
        if (m2s_frame) pmm_free_frame(m2s_frame);
        if (s2m_frame) pmm_free_frame(s2m_frame);
        if (pty_frame) pmm_free_frame(pty_frame);
    }
    k_memset(ent, 0, sizeof(*ent));
}

static void process_handle_socket_close(fd_entry_t* ent) {
    if (!ent) return;
    socket_release(ent->socket);
    k_memset(ent, 0, sizeof(*ent));
}

static int special_wait_object_kind(int kind) {
    return kind == PROCESS_HANDLE_KIND_TIMERFD ||
           kind == PROCESS_HANDLE_KIND_SIGNALFD;
}

static special_wait_object_t* special_wait_object(fd_entry_t* ent, int create) {
    special_wait_object_t* obj;

    if (!ent || !special_wait_object_kind(ent->kind)) return 0;

    if (!ent->aux_frame && create) {
        ent->aux_frame = pmm_alloc_frame();
        if (!ent->aux_frame) return 0;
        obj = (special_wait_object_t*)paging_phys_to_kernel_virt(ent->aux_frame);
        k_memset(obj, 0, PAGE_SIZE);
        wait_queue_init(&obj->read_waiters);
        return obj;
    }

    if (!ent->aux_frame) return 0;
    return (special_wait_object_t*)paging_phys_to_kernel_virt(ent->aux_frame);
}

static int special_wait_readable(fd_entry_t* ent, process_t* proc) {
    special_wait_object_t* obj;

    if (!ent || !proc) return -EINVAL;
    obj = special_wait_object(ent, 1);
    if (!obj) return -ENOMEM;
    return wait_queue_add(&obj->read_waiters, proc);
}

static unsigned int signal_bit(int signum) {
    if (signum <= 0 || signum >= 32) return 0u;
    return 1u << (unsigned int)signum;
}

int process_user_fault_signal(process_t* proc,
                              unsigned int frame_esp,
                              unsigned int vector,
                              unsigned int has_err,
                              unsigned int err,
                              unsigned int signal,
                              unsigned int code,
                              unsigned int fault_addr) {
    unsigned int* frame = (unsigned int*)frame_esp;
    unsigned int bit = signal_bit((int)signal);
    process_sigaction_t* action;
    unsigned int handler;
    unsigned int eip_idx = has_err ? 13u : 12u;
    unsigned int cs_idx = has_err ? 14u : 13u;
    unsigned int eflags_idx = has_err ? 15u : 14u;
    unsigned int uesp_idx = has_err ? 16u : 15u;
    unsigned int ss_idx = has_err ? 17u : 16u;
    unsigned int old_mask;
    unsigned int user_esp;
    unsigned int frame_addr;
    sys_signal_frame_t user_frame;

    if (!proc || !frame || signal == 0u || signal >= 32u || bit == 0u) return 0;
    action = &proc->signal_actions[signal];
    handler = (action->flags & SYS_SA_SIGINFO) ? action->sigaction : action->handler;
    if (handler == 0u || handler == 1u || action->restorer == 0u) return 0;
    if ((frame[cs_idx] & 3u) != 3u) return 0;

    old_mask = proc->signal_mask;
    user_esp = frame[uesp_idx];
    frame_addr = (user_esp - sizeof(user_frame)) & ~0xFu;

    k_memset(&user_frame, 0, sizeof(user_frame));
    user_frame.retaddr = action->restorer;
    user_frame.signum = signal;
    user_frame.siginfo = frame_addr + offsetof(sys_signal_frame_t, info);
    user_frame.ucontext = frame_addr + offsetof(sys_signal_frame_t, context);
    user_frame.info.si_signo = (int)signal;
    user_frame.info.si_code = (int)code;
    user_frame.info.si_addr = fault_addr;
    user_frame.context.uc_sigmask = old_mask;
    user_frame.context.gregs[SYS_REG_GS] = (int)frame[0];
    user_frame.context.gregs[SYS_REG_FS] = (int)frame[1];
    user_frame.context.gregs[SYS_REG_ES] = (int)frame[2];
    user_frame.context.gregs[SYS_REG_DS] = (int)frame[3];
    user_frame.context.gregs[SYS_REG_EDI] = (int)frame[4];
    user_frame.context.gregs[SYS_REG_ESI] = (int)frame[5];
    user_frame.context.gregs[SYS_REG_EBP] = (int)frame[6];
    user_frame.context.gregs[SYS_REG_ESP] = (int)frame[7];
    user_frame.context.gregs[SYS_REG_EBX] = (int)frame[8];
    user_frame.context.gregs[SYS_REG_EDX] = (int)frame[9];
    user_frame.context.gregs[SYS_REG_ECX] = (int)frame[10];
    user_frame.context.gregs[SYS_REG_EAX] = (int)frame[11];
    user_frame.context.gregs[SYS_REG_TRAPNO] = (int)vector;
    user_frame.context.gregs[SYS_REG_ERR] = (int)err;
    user_frame.context.gregs[SYS_REG_EIP] = (int)frame[eip_idx];
    user_frame.context.gregs[SYS_REG_CS] = (int)frame[cs_idx];
    user_frame.context.gregs[SYS_REG_EFL] = (int)frame[eflags_idx];
    user_frame.context.gregs[SYS_REG_UESP] = (int)frame[uesp_idx];
    user_frame.context.gregs[SYS_REG_SS] = (int)frame[ss_idx];

    if (copy_to_user((void*)frame_addr, &user_frame, sizeof(user_frame)) < 0) {
        return 0;
    }

    proc->signal_mask = old_mask | action->mask | bit;
    frame[eip_idx] = handler;
    frame[uesp_idx] = frame_addr;
    return 1;
}

static int signalfd_ready(fd_entry_t* ent) {
    special_wait_object_t* obj;

    if (!ent || ent->kind != PROCESS_HANDLE_KIND_SIGNALFD) return 0;
    obj = special_wait_object(ent, 0);
    return obj && (obj->pending_signals & obj->signal_mask) != 0u;
}

static int signalfd_consume(fd_entry_t* ent, kernel_signalfd_siginfo_t* out) {
    special_wait_object_t* obj;
    unsigned int pending;

    if (!ent || !out || ent->kind != PROCESS_HANDLE_KIND_SIGNALFD) return -EINVAL;
    obj = special_wait_object(ent, 0);
    if (!obj) return -EAGAIN;

    pending = obj->pending_signals & obj->signal_mask;
    if (pending == 0u) return -EAGAIN;

    for (unsigned int signum = 1u; signum < 32u; signum++) {
        unsigned int bit = 1u << signum;
        if ((pending & bit) == 0u) continue;

        obj->pending_signals &= ~bit;
        k_memset(out, 0, sizeof(*out));
        out->ssi_signo = signum;
        return 0;
    }

    return -EAGAIN;
}

static int timerfd_ready_at(fd_entry_t* ent, unsigned int now) {
    if (!ent || ent->kind != PROCESS_HANDLE_KIND_TIMERFD) return 0;
    return ent->timer_deadline != 0u &&
           (int)(now - ent->timer_deadline) >= 0;
}

static int timerfd_ready(fd_entry_t* ent) {
    return timerfd_ready_at(ent, timer_get_ticks());
}

static unsigned long long timerfd_consume(fd_entry_t* ent) {
    unsigned int now;
    unsigned int elapsed;
    unsigned int expirations;

    if (!timerfd_ready(ent)) return 0;
    now = timer_get_ticks();
    expirations = 1u;

    if (ent->timer_interval != 0u) {
        elapsed = now - ent->timer_deadline;
        expirations += elapsed / ent->timer_interval;
        ent->timer_deadline += expirations * ent->timer_interval;
    } else {
        ent->timer_deadline = 0u;
    }

    return (unsigned long long)expirations;
}

static int process_handle_special_read(fd_entry_t* ent, char* buf, unsigned int len) {
    unsigned long long expirations;
    process_t* proc;

    if (!ent || !ent->valid) return -EBADF;
    if (!buf) return -EFAULT;

    if (ent->kind == PROCESS_HANDLE_KIND_TIMERFD) {
        if (len < sizeof(unsigned long long)) return -EINVAL;

        if (!timerfd_ready(ent) &&
            (ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            return -EAGAIN;
        }

        proc = sched_current();
        while (!timerfd_ready(ent)) {
            int wait_rc;

            if (!proc) {
                __asm__ __volatile__("sti; hlt; cli");
                continue;
            }

            proc->state = PROCESS_STATE_WAITING;
            wait_rc = special_wait_readable(ent, proc);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                wait_queue_remove_proc(proc);
                return wait_rc;
            }
            if (timerfd_ready(ent)) {
                proc->state = PROCESS_STATE_RUNNING;
                break;
            }

            __asm__ __volatile__("sti");
            while (proc->state != PROCESS_STATE_RUNNING) {
                __asm__ __volatile__("hlt");
            }
            __asm__ __volatile__("cli");
            wait_queue_remove_proc(proc);
        }
        wait_queue_remove_proc(proc);

        expirations = timerfd_consume(ent);
        k_memcpy(buf, &expirations, sizeof(expirations));
        return (int)sizeof(expirations);
    }

    if (ent->kind == PROCESS_HANDLE_KIND_SIGNALFD) {
        kernel_signalfd_siginfo_t info;

        if (len < sizeof(info)) return -EINVAL;

        if (!signalfd_ready(ent) &&
            (ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
            return -EAGAIN;
        }

        proc = sched_current();
        while (!signalfd_ready(ent)) {
            int wait_rc;

            if (!proc) {
                __asm__ __volatile__("sti; hlt; cli");
                continue;
            }

            proc->state = PROCESS_STATE_WAITING;
            wait_rc = special_wait_readable(ent, proc);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                wait_queue_remove_proc(proc);
                return wait_rc;
            }
            if (signalfd_ready(ent)) {
                proc->state = PROCESS_STATE_RUNNING;
                break;
            }

            __asm__ __volatile__("sti");
            while (proc->state != PROCESS_STATE_RUNNING) {
                __asm__ __volatile__("hlt");
            }
            __asm__ __volatile__("cli");
            wait_queue_remove_proc(proc);
        }
        wait_queue_remove_proc(proc);

        if (signalfd_consume(ent, &info) < 0) {
            return -EAGAIN;
        }
        k_memcpy(buf, &info, sizeof(info));
        return (int)sizeof(info);
    }

    return -EINVAL;
}

static int process_handle_special_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    (void)ent;
    (void)buf;
    (void)len;
    return -EBADF;
}

static int process_handle_special_seek(fd_entry_t* ent, int offset, int whence) {
    (void)ent;
    (void)offset;
    (void)whence;
    return -EINVAL;
}

static short process_handle_special_poll(fd_entry_t* ent, short events) {
    short revents = 0;

    if (!ent || !ent->valid) return POLLERR;

    if (ent->kind == PROCESS_HANDLE_KIND_TIMERFD) {
        if ((events & POLLIN) && timerfd_ready(ent)) {
            revents |= POLLIN;
        }
        return revents;
    }

    if (ent->kind == PROCESS_HANDLE_KIND_SIGNALFD) {
        if ((events & POLLIN) && signalfd_ready(ent)) {
            revents |= POLLIN;
        }
        return revents;
    }

    return POLLERR;
}

static void process_handle_special_close(fd_entry_t* ent) {
    if (!ent) return;
    if (special_wait_object_kind(ent->kind)) {
        special_wait_object_t* obj = special_wait_object(ent, 0);
        if (obj) {
            wait_queue_wake_all(&obj->read_waiters);
        }
    }
    if (ent->aux_frame) {
        pmm_free_frame(ent->aux_frame);
    }
    k_memset(ent, 0, sizeof(*ent));
}

static int process_handle_virtual_read(fd_entry_t* ent, char* buf, unsigned int len) {
    virtual_object_t* obj = virtual_object_from_ent(ent);

    if (!ent || !ent->valid || !obj || !buf) return -EFAULT;
    if (!ent->readable) return -EBADF;
    if (len == 0u) return 0;

    if (obj->type == PROCESS_VIRTUAL_NULL) {
        return 0;
    }
    if (obj->type == PROCESS_VIRTUAL_ZERO) {
        k_memset(buf, 0, len);
        return (int)len;
    }
    if (obj->type == PROCESS_VIRTUAL_TTY) {
        return process_handle_console_read(ent, buf, len);
    }
    if (obj->type == PROCESS_VIRTUAL_URANDOM) {
        random_get_bytes(buf, len);
        return (int)len;
    }

    if (ent->offset >= ent->size) return 0;
    if (len > ent->size - ent->offset) {
        len = ent->size - ent->offset;
    }
    if (len == 0u) return 0;
    if (!obj->data_frame) return -EIO;

    k_memcpy(buf,
             ((const char*)paging_phys_to_kernel_virt(obj->data_frame)) + ent->offset,
             len);
    ent->offset += len;
    return (int)len;
}

static int process_handle_virtual_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    virtual_object_t* obj = virtual_object_from_ent(ent);

    if (!ent || !ent->valid || !obj || (!buf && len > 0u)) return -EFAULT;
    if (!ent->writable) return -EBADF;
    if (obj->type == PROCESS_VIRTUAL_NULL) return (int)len;
    if (obj->type == PROCESS_VIRTUAL_TTY) return process_handle_console_write(ent, buf, len);
    return -EBADF;
}

static int process_handle_virtual_seek(fd_entry_t* ent, int offset, int whence) {
    virtual_object_t* obj = virtual_object_from_ent(ent);
    int base;
    int next;

    if (!ent || !ent->valid || !obj) return -EBADF;
    if (obj->type == PROCESS_VIRTUAL_TTY ||
        obj->type == PROCESS_VIRTUAL_URANDOM) {
        return -EINVAL;
    }

    if (whence == 0) {
        base = 0;
    } else if (whence == 1) {
        base = (int)ent->offset;
    } else if (whence == 2) {
        base = (int)ent->size;
    } else {
        return -EINVAL;
    }

    next = base + offset;
    if (next < 0) return -EINVAL;
    ent->offset = (u32)next;
    return next;
}

static short process_handle_virtual_poll(fd_entry_t* ent, short events) {
    virtual_object_t* obj = virtual_object_from_ent(ent);
    short revents = 0;

    if (!ent || !ent->valid || !obj) return POLLERR;
    if ((events & POLLIN) && ent->readable) {
        if (obj->type == PROCESS_VIRTUAL_REGULAR) {
            if (ent->offset < ent->size) revents |= POLLIN;
        } else {
            revents |= POLLIN;
        }
    }
    if ((events & POLLOUT) && ent->writable) {
        revents |= POLLOUT;
    }
    return revents;
}

static void process_handle_virtual_close(fd_entry_t* ent) {
    virtual_object_t* obj = virtual_object_from_ent(ent);

    if (!ent) return;
    if (obj) {
        if (obj->refs > 1u) {
            obj->refs--;
        } else {
            if (obj->data_frame) pmm_free_frame(obj->data_frame);
            pmm_free_frame(ent->aux_frame);
        }
    }
    k_memset(ent, 0, sizeof(*ent));
}

int process_fd_read(fd_entry_t* ent, char* buf, unsigned int len) {
    if (!ent || !ent->ops) return -EBADF;
    if (!ent->ops->read) return -ENOSYS;
    return ent->ops->read(ent, buf, len);
}

int process_fd_read_raw(fd_entry_t* ent, char* buf, unsigned int len) {
    if (!ent || !ent->ops) return -EBADF;
    if (ent->kind == PROCESS_HANDLE_KIND_CONSOLE) {
        return process_handle_console_read_common(ent, buf, len, 0);
    }
    if (ent->kind == PROCESS_HANDLE_KIND_PTY_SLAVE) {
        return process_handle_pty_read_common(ent, buf, len, 0);
    }
    return process_fd_read(ent, buf, len);
}

int process_fd_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    if (!ent || !ent->ops) return -EBADF;
    if (!ent->ops->write) return -ENOSYS;
    return ent->ops->write(ent, buf, len);
}

short process_fd_poll(fd_entry_t* ent, short events) {
    if (!ent || !ent->ops || !ent->ops->poll) return POLLERR;
    return ent->ops->poll(ent, events);
}

int process_fd_wait(fd_entry_t* ent, process_t* proc, short events) {
    int rc;

    if (!ent || !ent->valid) return -EBADF;
    if (!proc) return -EINVAL;

    if (ent->kind == PROCESS_HANDLE_KIND_SOCKET) {
        return socket_wait(ent->socket, proc, events);
    }

    if (special_wait_object_kind(ent->kind)) {
        if ((events & POLLIN) == 0) return 0;
        rc = special_wait_readable(ent, proc);
        if (rc < 0) {
            wait_queue_remove_proc(proc);
        }
        return rc;
    }

    if (ent->kind == PROCESS_HANDLE_KIND_CONSOLE) {
        if ((events & POLLIN) && ent->readable) {
            keyboard_set_waiting_process(proc);
        }
        return 0;
    }

    if (ent->kind == PROCESS_HANDLE_KIND_PIPE) {
        pipe_object_t* pipe = pipe_object_from_ent(ent);
        if (!pipe) return -EIO;
        if ((events & POLLIN) && ent->readable) {
            rc = wait_queue_add(&pipe->read_waiters, proc);
            if (rc < 0) {
                wait_queue_remove_proc(proc);
                return rc;
            }
        }
        if ((events & POLLOUT) && ent->writable) {
            rc = wait_queue_add(&pipe->write_waiters, proc);
            if (rc < 0) {
                wait_queue_remove_proc(proc);
                return rc;
            }
        }
        return 0;
    }

    if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER ||
        ent->kind == PROCESS_HANDLE_KIND_PTY_SLAVE) {
        pty_object_t* pty = pty_object_from_ent(ent);
        pty_buffer_t* read_buf;
        pty_buffer_t* write_buf;
        if (!pty) return -EIO;
        if (ent->kind == PROCESS_HANDLE_KIND_PTY_MASTER) {
            read_buf = &pty->slave_to_master;
            write_buf = &pty->master_to_slave;
        } else {
            read_buf = &pty->master_to_slave;
            write_buf = &pty->slave_to_master;
        }
        if ((events & POLLIN) && ent->readable) {
            rc = wait_queue_add(&read_buf->read_waiters, proc);
            if (rc < 0) {
                wait_queue_remove_proc(proc);
                return rc;
            }
        }
        if ((events & POLLOUT) && ent->writable) {
            rc = wait_queue_add(&write_buf->write_waiters, proc);
            if (rc < 0) {
                wait_queue_remove_proc(proc);
                return rc;
            }
        }
        return 0;
    }

    return 0;
}

int process_fd_seek(fd_entry_t* ent, int offset, int whence) {
    if (!ent || !ent->ops) return -EBADF;
    if (!ent->ops->seek) return -ENOSYS;
    return ent->ops->seek(ent, offset, whence);
}

int process_fd_flush(fd_entry_t* ent) {
    if (!ent || !ent->ops || !ent->ops->flush) return 1;
    return ent->ops->flush(ent);
}

int process_fd_set_flags(fd_entry_t* ent, unsigned int flags) {
    if (!ent || !ent->valid) return -EBADF;
    ent->flags = flags & SYS_FD_FLAG_NONBLOCK;
    return 0;
}

unsigned int process_fd_get_flags(fd_entry_t* ent) {
    if (!ent || !ent->valid) return 0;
    if (ent->readable && ent->writable) return ent->flags | 2u;
    if (ent->writable) return ent->flags | 1u;
    return ent->flags;
}

int process_fd_set_fd_flags(fd_entry_t* ent, unsigned int flags) {
    if (!ent || !ent->valid) return -EBADF;
    ent->fd_flags = flags & SYS_FD_FLAG_CLOEXEC;
    return 0;
}

unsigned int process_fd_get_fd_flags(fd_entry_t* ent) {
    if (!ent || !ent->valid) return 0;
    return ent->fd_flags;
}

void process_close_cloexec_fds(process_t* proc) {
    if (!proc || !proc->fds) return;
    for (unsigned int i = 0; i < proc->fd_capacity; i++) {
        fd_entry_t* ent = &proc->fds[i];
        if (ent->valid && (ent->fd_flags & SYS_FD_FLAG_CLOEXEC) != 0u) {
            process_fd_close(ent);
        }
    }
}

int process_copy_fd_table(process_t* dst, process_t* src) {
    if (!dst || !src || !dst->fds || !src->fds) return -EINVAL;
    while (dst->fd_capacity < src->fd_capacity) {
        int rc = process_fd_table_grow(dst, src->fd_capacity);
        if (rc < 0) return rc;
    }

    for (unsigned int i = 0; i < dst->fd_capacity; i++) {
        if (dst->fds[i].valid) {
            process_fd_close(&dst->fds[i]);
        }
    }
    k_memset(dst->fds, 0, dst->fd_table_frames * PAGE_SIZE);

    for (unsigned int i = 0; i < src->fd_capacity; i++) {
        if (!src->fds[i].valid) continue;
        dst->fds[i] = src->fds[i];
        process_fd_share_ref(&dst->fds[i]);
    }
    return 0;
}

int process_fd_set_signalfd_mask(fd_entry_t* ent, unsigned int mask) {
    special_wait_object_t* obj;

    if (!ent || !ent->valid || ent->kind != PROCESS_HANDLE_KIND_SIGNALFD) {
        return -EBADF;
    }

    obj = special_wait_object(ent, 1);
    if (!obj) return -ENOMEM;
    obj->signal_mask = mask;
    obj->pending_signals &= mask;
    return 0;
}

void process_wake_timerfds(process_t* proc, unsigned int now) {
    if (!process_fd_table_is_valid(proc)) return;

    for (unsigned int i = 0; i < proc->fd_capacity; i++) {
        fd_entry_t* ent = &proc->fds[i];
        special_wait_object_t* obj;

        if (!ent->valid || ent->kind != PROCESS_HANDLE_KIND_TIMERFD) {
            continue;
        }
        if (!timerfd_ready_at(ent, now)) continue;

        obj = special_wait_object(ent, 0);
        if (!obj) continue;
        wait_queue_wake_all(&obj->read_waiters);
    }
}

void process_claim_for_wait(process_t* proc) {
    if (!proc) return;
    proc->reaper_claimed = 1;
}

int process_wait_pid(process_t* parent,
                     int pid,
                     int options,
                     int* out_pid,
                     int* out_status) {
    process_t* child;

    if (!parent || !out_pid || !out_status) return -EINVAL;
    if (pid == 0 || pid < -1) return -EINVAL;
    if ((options & ~(SYS_WAITPID_WNOHANG | SYS_WAITPID_WUNTRACED)) != 0) return -EINVAL;

    child = process_find_child(parent, pid);
    if (!child) return -ECHILD;

    while (1) {
        child = process_find_zombie_child(parent, pid);
        if (child) {
            int child_pid = (int)child->pid;
            int status = child->exit_status;

            *out_pid = child_pid;
            *out_status = status;
            parent->child_cpu_ticks += child->cpu_ticks + child->child_cpu_ticks;
            parent->child_wait_count++;
            sched_dequeue(child);
            process_destroy(child);
            return 0;
        }

        if ((options & SYS_WAITPID_WUNTRACED) != 0) {
            child = process_find_stopped_child(parent, pid);
            if (child) {
                *out_pid = (int)child->pid;
                *out_status = child->exit_status;
                child->stop_reported = 1;
                return 0;
            }
        }

        if (options & SYS_WAITPID_WNOHANG) {
            *out_pid = 0;
            *out_status = 0;
            return 0;
        }

        parent->state = PROCESS_STATE_WAITING;
        __asm__ __volatile__("sti; hlt; cli");
    }
}

int process_kill_pid(int pid, int status, unsigned int esp) {
    process_t* proc;

    if (pid <= 0) return -EINVAL;

    proc = process_find_by_pid((u32)pid);
    if (!proc || proc->state == PROCESS_STATE_ZOMBIE ||
        proc->state == PROCESS_STATE_EXITED) {
        return -ESRCH;
    }
    if (!proc->pd) {
        return -EPERM;
    }

    proc->exit_status = status;
    if (keyboard_get_waiting_process() == (void*)proc) {
        keyboard_set_waiting_process(0);
    }
    if (s_raw_console_reader == proc) {
        s_raw_console_reader = 0;
    }
    process_clear_display_input_owner(proc);
    input_forget_waiting_process(proc);
    socket_wait_clear_process(proc);

    if (proc == sched_current()) {
        paging_switch(paging_get_kernel_pd());
    }
    sched_kill(proc, esp);
    return 0;
}

static int process_stop_one(process_t* proc,
                            int signum,
                            process_t* defer_current,
                            int mark_terminal_stop) {
    if (!proc || proc->state == PROCESS_STATE_ZOMBIE ||
        proc->state == PROCESS_STATE_EXITED ||
        proc->state == PROCESS_STATE_STOPPED) {
        return 0;
    }
    if (!proc->pd) {
        return 0;
    }

    proc->exit_status = PROCESS_STOPPED_STATUS(signum);
    proc->stop_reported = 0;
    if (keyboard_get_waiting_process() == (void*)proc) {
        keyboard_set_waiting_process(0);
    }
    if (s_raw_console_reader == proc) {
        s_raw_console_reader = 0;
    }
    process_clear_display_input_owner(proc);
    input_forget_waiting_process(proc);
    socket_wait_clear_process(proc);
    wait_queue_remove_proc(proc);
    proc->state = PROCESS_STATE_STOPPED;
    process_wake_parent_waiter(proc);

    if (proc == defer_current && mark_terminal_stop) {
        s_terminal_stop_target = proc;
        s_terminal_stop_pending = 1;
    }
    return 1;
}

int process_stop_pid(int pid, int signum, unsigned int esp) {
    process_t* proc;

    if (pid <= 0) return -EINVAL;

    proc = process_find_by_pid((u32)pid);
    if (!proc || proc->state == PROCESS_STATE_ZOMBIE ||
        proc->state == PROCESS_STATE_EXITED) {
        return -ESRCH;
    }
    if (!proc->pd) {
        return -EPERM;
    }

    (void)process_stop_one(proc, signum, sched_current(), 0);
    if (proc == sched_current()) {
        sched_yield_now(esp);
    }
    return 0;
}

int process_continue_pid(int pid) {
    process_t* proc;

    if (pid <= 0) return -EINVAL;

    proc = process_find_by_pid((u32)pid);
    if (!proc || proc->state == PROCESS_STATE_ZOMBIE ||
        proc->state == PROCESS_STATE_EXITED) {
        return -ESRCH;
    }
    if (!proc->pd) {
        return -EPERM;
    }

    (void)process_signal_deliver(proc, PROCESS_SIGCONT);
    if (proc->state == PROCESS_STATE_STOPPED) {
        proc->stop_reported = 0;
        proc->state = PROCESS_STATE_RUNNING;
    }
    return 0;
}

int process_reap_unclaimed_zombies(void) {
    int reaped = 0;
    process_t* current = sched_current();

    if (!s_process_registry) return 0;

    for (int i = (int)s_process_registry_capacity - 1; i >= 0; i--) {
        process_t* proc = s_process_registry[i];

        if (!proc) continue;
        if (proc == current) continue;
        if (proc->state != PROCESS_STATE_ZOMBIE) continue;
        if (proc->reaper_claimed) continue;

        sched_dequeue(proc);
        process_destroy(proc);
        reaped++;
    }

    return reaped;
}

/*
 * Copy launch argv into process-owned storage.  The incoming argv may point at
 * shell parser storage or at a temporary SYS_EXEC validation buffer; after this
 * returns, bootstrap uses only proc->user_arg_data/user_argv.
 */
int process_set_args(process_t* proc, int argc, char** argv) {
    unsigned int used = 0;
    int rc;

    if (!proc) return -EINVAL;
    if (argc < 0 || argc > PROCESS_MAX_ARGS) return -EINVAL;
    if (argc > 0 && !argv) return -EFAULT;

    for (int i = 0; i < argc; i++) {
        int len;

        if (!argv[i]) return -EFAULT;

        len = k_strlen(argv[i]) + 1;
        if (used + (unsigned int)len > PROCESS_ARG_BYTES) {
            return -EINVAL;
        }
        used += (unsigned int)len;
    }

    if (argc > 0) {
        rc = process_string_arena_ensure(&proc->user_arg_data,
                                         &proc->user_arg_frame,
                                         &proc->user_arg_frames,
                                         PROCESS_ARG_BYTES);
        if (rc < 0) return rc;
    }

    proc->user_argc = 0;
    for (int i = 0; i <= PROCESS_MAX_ARGS; i++) proc->user_argv[i] = 0;
    if (proc->user_arg_data) {
        k_memset(proc->user_arg_data, 0, PROCESS_ARG_BYTES);
    }

    used = 0;
    for (int i = 0; i < argc; i++) {
        int len = k_strlen(argv[i]) + 1;
        proc->user_argv[i] = &proc->user_arg_data[used];
        k_memcpy(proc->user_argv[i], argv[i], (k_size_t)len);
        used += (unsigned int)len;
    }

    proc->user_argc = argc;
    proc->user_argv[argc] = 0;
    return 0;
}

int process_set_env(process_t* proc, int envc, char** envp) {
    unsigned int used = 0;
    int rc;

    if (!proc) return -EINVAL;
    if (envc < 0 || envc > PROCESS_MAX_ENVS) return -EINVAL;
    if (envc > 0 && !envp) return -EFAULT;

    for (int i = 0; i < envc; i++) {
        int len;

        if (!envp[i]) return -EFAULT;

        len = k_strlen(envp[i]) + 1;
        if (used + (unsigned int)len > PROCESS_ENV_BYTES) {
            return -EINVAL;
        }
        used += (unsigned int)len;
    }

    if (envc > 0) {
        rc = process_string_arena_ensure(&proc->user_env_data,
                                         &proc->user_env_frame,
                                         &proc->user_env_frames,
                                         PROCESS_ENV_BYTES);
        if (rc < 0) return rc;
    }

    proc->user_envc = 0;
    for (int i = 0; i <= PROCESS_MAX_ENVS; i++) proc->user_envp[i] = 0;
    if (proc->user_env_data) {
        k_memset(proc->user_env_data, 0, PROCESS_ENV_BYTES);
    }

    used = 0;
    for (int i = 0; i < envc; i++) {
        int len = k_strlen(envp[i]) + 1;
        proc->user_envp[i] = &proc->user_env_data[used];
        k_memcpy(proc->user_envp[i], envp[i], (k_size_t)len);
        used += (unsigned int)len;
    }

    proc->user_envc = envc;
    proc->user_envp[envc] = 0;
    return 0;
}

int process_set_auxv(process_t* proc, int auxc, const unsigned int* auxv_pairs) {
    if (!proc) return -EINVAL;
    if (auxc < 0 || auxc > PROCESS_AUXV_MAX) return -EINVAL;
    if (auxc > 0 && !auxv_pairs) return -EFAULT;

    proc->user_auxc = auxc;
    for (int i = 0; i < auxc * 2; i++) {
        proc->user_auxv[i] = auxv_pairs[i];
    }
    proc->user_auxv[auxc * 2] = 0;
    proc->user_auxv[auxc * 2 + 1] = 0;
    return 0;
}

int process_set_default_env(process_t* proc) {
    char* envp[] = {
        "PATH=/bin:/usr/bin:/usr/sbin",
        "HOME=/",
        "SHELL=/bin/shell",
        "TMPDIR=/tmp",
        0
    };

    return process_set_env(proc, 4, envp);
}

/* ------------------------------------------------------------------ */
/* Kernel-task bootstrap                                              */
/* ------------------------------------------------------------------ */

static void process_kernel_task_bootstrap(void) {
    process_t* proc = sched_current();

    if (!proc || !proc->kernel_entry) {
        terminal_puts("process: kernel task bootstrap failed\n");
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }

    __asm__ __volatile__("sti");
    proc->kernel_entry();

    terminal_puts("process: kernel task returned\n");
    for (;;) {
        __asm__ __volatile__("cli; hlt");
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                         */
/* ------------------------------------------------------------------ */

process_t* process_create(const char* name) {
    u32 frame = pmm_alloc_frame();
    int fd_rc;

    if (!frame) {
        terminal_puts("process: out of frames for process_t\n");
        return 0;
    }

    process_t* proc = (process_t*)paging_phys_to_kernel_virt(frame);
    proc_zero(proc);

    proc->pid = process_alloc_pid();
    if (proc->pid == 0u) {
        terminal_puts("process: out of pids\n");
        pmm_free_frame(frame);
        return 0;
    }
    {
        process_t* parent = sched_current();
        proc->parent_pid = parent ? parent->pid : 0;
        if (parent) {
            proc->sid = parent->sid ? parent->sid : parent->pid;
            proc->session_leader = 0;
            proc->uid = parent->uid;
            proc->gid = parent->gid;
            proc->euid = parent->euid;
            proc->egid = parent->egid;
            proc->supp_gid = parent->supp_gid;
            proc->supp_gid_count = parent->supp_gid_count;
            proc->umask = parent->umask;
        } else {
            proc->sid = proc->pid;
            proc->session_leader = 1;
            proc->uid = 0;
            proc->gid = 0;
            proc->euid = 0;
            proc->egid = 0;
            proc->supp_gid = 0;
            proc->supp_gid_count = 1;
            proc->umask = 0022u;
        }
    }
    proc->state = PROCESS_STATE_UNUSED;
    proc->heap_base = USER_HEAP_BASE;
    proc->heap_brk = USER_HEAP_BASE;
    proc->mmap_base = USER_MMAP_BASE;
    proc->mmap_next = USER_MMAP_BASE;
    fd_rc = process_fd_table_init(proc, PROCESS_FD_LIMIT_DEFAULT);
    if (fd_rc < 0) {
        terminal_puts("process: out of frames for fd table\n");
        pmm_free_frame(frame);
        return 0;
    }
    if (!process_vm_init(proc)) {
        terminal_puts("process: out of frames for vm table\n");
        process_fd_table_free(proc);
        pmm_free_frame(frame);
        return 0;
    }
    process_init_standard_fds(proc);
    if (process_set_default_env(proc) < 0) {
        terminal_puts("process: out of frames for environment\n");
        process_launch_context_free(proc);
        process_fd_table_free(proc);
        process_vm_free(proc);
        pmm_free_frame(frame);
        return 0;
    }
    if (name) {
        str_copy_n(proc->name, name, PROCESS_NAME_MAX);
    }

    if (!process_registry_add(proc)) {
        process_launch_context_free(proc);
        process_fd_table_free(proc);
        process_vm_free(proc);
        pmm_free_frame(frame);
        return 0;
    }

    return proc;
}

process_t* process_create_kernel_task(const char* name, void (*entry)(void)) {
    process_t* proc = process_create(name);
    if (!proc) return 0;

    proc->kernel_stack_frames = PROCESS_KERNEL_STACK_FRAMES;
    proc->kernel_stack_frame = pmm_alloc_contiguous_frames(proc->kernel_stack_frames);
    if (!proc->kernel_stack_frame) {
        terminal_puts("process: out of frames for kernel stack\n");
        process_destroy(proc);
        return 0;
    }
    k_memset(paging_phys_to_kernel_virt(proc->kernel_stack_frame),
             0,
             proc->kernel_stack_frames * PAGE_SIZE);

    proc->pd = 0;
    proc->kernel_entry = entry;
    proc->state = PROCESS_STATE_RUNNING;

    {
        unsigned int* stack_top =
            (unsigned int*)((u8*)paging_phys_to_kernel_virt(proc->kernel_stack_frame) +
                            proc->kernel_stack_frames * PAGE_SIZE);
        stack_top--;
        *stack_top = (unsigned int)process_kernel_task_bootstrap;
        proc->sched_esp = (unsigned int)stack_top;
    }

    return proc;
}

process_t* process_fork_from_syscall(unsigned int regs_esp, unsigned int frame_top) {
    process_t* parent = sched_current();
    process_t* child;
    unsigned int frame_bytes;
    unsigned int child_stack_top;
    unsigned int copy_start;
    unsigned int child_copy_start;
    unsigned int child_regs_esp;

    if (!parent || !parent->pd || !parent->kernel_stack_frame) return 0;
    if (regs_esp < 8u || regs_esp >= frame_top) return 0;

    child = process_create(parent->name);
    if (!child) return 0;

    if (!process_vm_clone(child, parent)) {
        process_destroy(child);
        return 0;
    }

    child->pd = process_pd_clone_user(parent->pd, process_vm_page_may_write, parent);
    if (!child->pd) {
        process_destroy(child);
        return 0;
    }

    child->kernel_stack_frames = parent->kernel_stack_frames ? parent->kernel_stack_frames
                                                             : PROCESS_KERNEL_STACK_FRAMES;
    child->kernel_stack_frame = pmm_alloc_contiguous_frames(child->kernel_stack_frames);
    if (!child->kernel_stack_frame) {
        process_destroy(child);
        return 0;
    }
    k_memset(paging_phys_to_kernel_virt(child->kernel_stack_frame),
             0,
             child->kernel_stack_frames * PAGE_SIZE);

    copy_start = regs_esp - 8u;
    frame_bytes = frame_top - copy_start;
    if (frame_bytes > child->kernel_stack_frames * PAGE_SIZE) {
        process_destroy(child);
        return 0;
    }

    child_stack_top = (unsigned int)paging_phys_to_kernel_virt(child->kernel_stack_frame) +
                      child->kernel_stack_frames * PAGE_SIZE;
    child_copy_start = child_stack_top - frame_bytes;
    child_regs_esp = child_copy_start + 8u;
    k_memcpy((void*)child_copy_start, (const void*)copy_start, frame_bytes);
    ((unsigned int*)child_copy_start)[1] = child_regs_esp;
    ((unsigned int*)child_regs_esp)[11] = 0u; /* saved eax: fork returns 0 in child */
    child->sched_esp = child_copy_start;

    child->heap_base = parent->heap_base;
    child->heap_brk = parent->heap_brk;
    child->mmap_base = parent->mmap_base;
    child->mmap_next = parent->mmap_next;
    child->pgid = parent->pgid;
    child->sid = parent->sid;
    child->session_leader = 0;
    child->uid = parent->uid;
    child->gid = parent->gid;
    child->euid = parent->euid;
    child->egid = parent->egid;
    child->supp_gid = parent->supp_gid;
    child->supp_gid_count = parent->supp_gid_count;
    child->umask = parent->umask;
    child->user_entry = parent->user_entry;
    child->user_stack_esp = parent->user_stack_esp;
    if (process_set_args(child, parent->user_argc, parent->user_argv) < 0 ||
        process_set_env(child, parent->user_envc, parent->user_envp) < 0) {
        process_destroy(child);
        return 0;
    }
    child->user_auxc = parent->user_auxc;
    k_memcpy(child->user_auxv, parent->user_auxv, sizeof(child->user_auxv));
    k_memcpy(child->cwd, parent->cwd, sizeof(child->cwd));

    if (process_copy_fd_table(child, parent) < 0) {
        process_destroy(child);
        return 0;
    }

    child->state = PROCESS_STATE_RUNNING;
    if (!sched_enqueue(child)) {
        process_destroy(child);
        return 0;
    }

    return child;
}

void process_destroy(process_t* proc) {
    if (!proc) return;

    /*
     * If this process is currently parked as the keyboard waiter, clear the
     * slot before freeing the process_t frame.  Leaving a dangling pointer
     * in the keyboard driver would cause process_key_consumer() to write
     * through freed memory on the next keypress.
     */
    if (keyboard_get_waiting_process() == (void*)proc) {
        keyboard_set_waiting_process(0);
    }
    if (s_raw_console_reader == proc) {
        s_raw_console_reader = 0;
    }
    process_clear_display_input_owner(proc);
    input_forget_waiting_process(proc);
    socket_wait_clear_process(proc);
    wait_queue_remove_proc(proc);
    display_release(proc);

    for (unsigned int i = 0; i < proc->fd_capacity; i++) {
        if (proc->fds[i].valid) {
            process_fd_close(&proc->fds[i]);
        }
    }
    process_fd_table_free(proc);

    if (proc->pd) {
        process_pd_destroy(proc->pd);
        proc->pd = 0;
    }
    process_vm_free(proc);
    process_launch_context_free(proc);

    if (proc->kernel_stack_frame) {
        u32 frames = proc->kernel_stack_frames ? proc->kernel_stack_frames : 1u;
        pmm_free_contiguous_frames(proc->kernel_stack_frame, frames);
        proc->kernel_stack_frame = 0;
        proc->kernel_stack_frames = 0;
    }

    if (s_foreground_reader == proc) {
        s_foreground_reader = 0;
        s_foreground_pgid = 0;
    }
    if (s_terminal_interrupt_target == proc) {
        s_terminal_interrupt_target = 0;
        s_terminal_interrupt_pending = 0;
    }

    process_orphan_children(proc->pid);
    process_registry_remove(proc);

    proc->state = PROCESS_STATE_EXITED;
    pmm_free_frame(paging_kernel_virt_to_phys(proc));
}

void process_release_exit_resources(process_t* proc) {
    if (!proc) return;

    for (unsigned int i = 0; i < proc->fd_capacity; i++) {
        if (proc->fds[i].valid) {
            process_fd_close(&proc->fds[i]);
        }
    }
    if (proc->pd) {
        process_pd_destroy(proc->pd);
        proc->pd = 0;
    }
    process_vm_free(proc);
}

process_t* process_get_current(void) {
    return sched_current();
}

void process_set_display_input_owner(process_t* proc, int enabled) {
    if (enabled) {
        if (proc) {
            s_display_input_owner = proc;
        }
        return;
    }

    if (!proc) {
        s_display_input_owner = 0;
        return;
    }

    if (s_display_input_owner == proc) {
        process_clear_display_input_owner(proc);
    }
}

void process_init_user_group(process_t* proc) {
    process_t* parent;

    if (!proc) return;

    parent = sched_current();
    if (parent && parent->sid != 0) {
        proc->sid = parent->sid;
        proc->session_leader = 0;
    } else if (proc->sid == 0) {
        proc->sid = proc->pid;
        proc->session_leader = 1;
    }
    if (parent && parent->pgid != 0) {
        proc->pgid = parent->pgid;
    } else {
        proc->pgid = proc->pid;
    }
}

void process_set_foreground(process_t* proc) {
    if (s_display_input_owner && s_display_input_owner != proc) {
        process_clear_display_input_owner(s_display_input_owner);
    }
    s_foreground_reader = proc;
    s_foreground_pgid = proc ? proc->pgid : 0;
    s_detach_requested = 0;
    keyboard_reset_modifiers();
    if (proc) {
        keyboard_buf_clear();           /* discard any input that arrived before
                                           the process was ready to read it,
                                           e.g. the Enter that launched the foreground program */
        input_clear_events();
        keyboard_set_consumer(process_key_consumer);
    } else {
        keyboard_set_consumer(process_key_consumer);
    }
}

void process_set_foreground_preserve_input(process_t* proc) {
    if (s_display_input_owner && s_display_input_owner != proc) {
        process_clear_display_input_owner(s_display_input_owner);
    }
    s_foreground_reader = proc;
    s_foreground_pgid = proc ? proc->pgid : 0;
    s_detach_requested = 0;
    keyboard_reset_modifiers();
    if (proc) {
        keyboard_set_consumer(process_key_consumer);
    } else {
        keyboard_set_consumer(process_key_consumer);
    }
}

process_t* process_get_foreground(void) {
    return s_foreground_reader;
}

u32 process_get_foreground_group(void) {
    return s_foreground_pgid;
}

int process_getsid(process_t* caller, int pid) {
    process_t* target;

    if (!caller) return -EINVAL;
    if (pid < 0) return -ESRCH;
    target = (pid == 0) ? caller : process_find_by_pid((u32)pid);
    if (!target) return -ESRCH;
    return (int)(target->sid ? target->sid : target->pid);
}

int process_setsid(process_t* proc) {
    if (!proc) return -EINVAL;
    if (proc->pgid == proc->pid) return -EPERM;

    proc->sid = proc->pid;
    proc->pgid = proc->pid;
    proc->session_leader = 1;
    if (s_foreground_reader == proc) {
        s_foreground_pgid = proc->pgid;
    }
    return (int)proc->sid;
}

int process_getpgid(process_t* caller, int pid) {
    process_t* target;

    if (!caller) return -EINVAL;
    if (pid < 0) return -ESRCH;
    target = (pid == 0) ? caller : process_find_by_pid((u32)pid);
    if (!target) return -ESRCH;
    return (int)target->pgid;
}

int process_setpgid(process_t* caller, int pid, int pgid) {
    process_t* target;
    process_t* group_member;
    u32 new_pgid;

    if (!caller) return -EINVAL;
    if (pid < 0 || pgid < 0) return -EINVAL;

    target = (pid == 0) ? caller : process_find_by_pid((u32)pid);
    if (!target) return -ESRCH;
    if (target != caller && target->parent_pid != caller->pid) return -ESRCH;
    if (target->sid != caller->sid) return -EPERM;
    if (target->session_leader) return -EPERM;

    new_pgid = (pgid == 0) ? target->pid : (u32)pgid;
    group_member = process_find_by_pgid(new_pgid);
    if (group_member && group_member->sid != caller->sid) return -EPERM;
    if (!group_member && new_pgid != target->pid) return -EPERM;

    target->pgid = new_pgid;
    if (s_foreground_reader == target) {
        s_foreground_pgid = target->pgid;
    }
    return 0;
}

int process_signal_deliver(process_t* proc, int signum) {
    unsigned int bit = signal_bit(signum);
    int delivered = 0;

    if (!proc || !proc->fds || bit == 0u) return 0;

    for (unsigned int i = 0; i < proc->fd_capacity; i++) {
        fd_entry_t* ent = &proc->fds[i];
        special_wait_object_t* obj;

        if (!ent->valid || ent->kind != PROCESS_HANDLE_KIND_SIGNALFD) {
            continue;
        }

        obj = special_wait_object(ent, 0);
        if (!obj || (obj->signal_mask & bit) == 0u) {
            continue;
        }

        obj->pending_signals |= bit;
        wait_queue_wake_all(&obj->read_waiters);
        delivered = 1;
    }

    return delivered;
}

int process_group_signal_deliver(u32 pgid, int signum) {
    process_t* targets[SCHED_MAX_PROCS];
    int count;
    int delivered = 0;

    if (pgid == 0) return 0;

    count = sched_snapshot_process_group(pgid, targets, SCHED_MAX_PROCS);
    for (int i = 0; i < count; i++) {
        if (process_signal_deliver(targets[i], signum)) {
            delivered = 1;
        }
    }

    return delivered;
}

static int process_group_force_exit(u32 pgid,
                                    int status,
                                    process_t* defer_current,
                                    int mark_terminal_interrupt) {
    process_t* targets[SCHED_MAX_PROCS];
    int count;
    int killed = 0;

    if (pgid == 0) return 0;

    count = sched_snapshot_process_group(pgid, targets, SCHED_MAX_PROCS);
    for (int i = 0; i < count; i++) {
        process_t* proc = targets[i];
        if (!proc || proc->state == PROCESS_STATE_ZOMBIE) {
            continue;
        }

        proc->exit_status = status;
        if (keyboard_get_waiting_process() == (void*)proc) {
            keyboard_set_waiting_process(0);
        }
        if (s_raw_console_reader == proc) {
            s_raw_console_reader = 0;
        }
        process_clear_display_input_owner(proc);
        input_forget_waiting_process(proc);
        socket_wait_clear_process(proc);

        if (proc == defer_current && mark_terminal_interrupt) {
            s_terminal_interrupt_target = proc;
            s_terminal_interrupt_pending = 1;
        } else {
            sched_kill(proc, 0);
        }
        killed = 1;
    }

    return killed;
}

int process_group_kill(u32 pgid, int status) {
    return process_group_force_exit(pgid, status, 0, 0);
}

int process_group_stop(u32 pgid,
                       int signum,
                       process_t* defer_current,
                       int mark_terminal_stop) {
    process_t* targets[SCHED_MAX_PROCS];
    int count;
    int stopped = 0;

    if (pgid == 0) return 0;

    count = sched_snapshot_process_group(pgid, targets, SCHED_MAX_PROCS);
    for (int i = 0; i < count; i++) {
        if (process_stop_one(targets[i], signum, defer_current, mark_terminal_stop)) {
            stopped = 1;
        }
    }
    return stopped;
}

int process_group_continue(u32 pgid) {
    process_t* targets[SCHED_MAX_PROCS];
    int count;
    int found = 0;

    if (pgid == 0) return -EINVAL;

    count = sched_snapshot_process_group(pgid, targets, SCHED_MAX_PROCS);
    for (int i = 0; i < count; i++) {
        process_t* proc = targets[i];
        if (!proc || proc->state == PROCESS_STATE_ZOMBIE ||
            proc->state == PROCESS_STATE_EXITED) {
            continue;
        }
        found = 1;
        (void)process_signal_deliver(proc, PROCESS_SIGCONT);
        if (proc->state == PROCESS_STATE_STOPPED) {
            proc->stop_reported = 0;
            proc->state = PROCESS_STATE_RUNNING;
        }
    }
    return found ? 0 : -ESRCH;
}

void process_deliver_pending_terminal_interrupt(unsigned int esp) {
    process_t* proc;

    if (s_terminal_stop_pending) {
        proc = s_terminal_stop_target ? s_terminal_stop_target : sched_current();
        s_terminal_stop_pending = 0;
        s_terminal_stop_target = 0;
        if (proc && proc == sched_current() &&
            proc->state == PROCESS_STATE_STOPPED) {
            sched_yield_now(esp);
        }
    }

    if (!s_terminal_interrupt_pending) return;

    proc = s_terminal_interrupt_target ? s_terminal_interrupt_target : sched_current();
    s_terminal_interrupt_pending = 0;
    s_terminal_interrupt_target = 0;
    if (!proc || proc != sched_current()) return;

    proc->exit_status = PROCESS_TERMINATED_BY_CTRL_C;
    if (keyboard_get_waiting_process() == (void*)proc) {
        keyboard_set_waiting_process(0);
    }
    if (s_raw_console_reader == proc) {
        s_raw_console_reader = 0;
    }
    process_clear_display_input_owner(proc);
    input_forget_waiting_process(proc);
    socket_wait_clear_process(proc);
    paging_switch(paging_get_kernel_pd());
    sched_kill(proc, esp);
}

static int process_wait_impl(process_t* proc, int allow_detach, int* detached) {
    if (!proc) return -1;

    if (detached) {
        *detached = 0;
    }
    s_detach_allowed = allow_detach ? 1 : 0;
    /*
     * bootseq may install a suspended shell as foreground before resuming it.
     * Preserve that handoff instead of clearing input a second time here.
     */
    if (process_get_foreground() == proc) {
        process_set_foreground_preserve_input(proc);
    } else {
        process_set_foreground(proc);
    }
    process_claim_for_wait(proc);

    while (proc->state != PROCESS_STATE_ZOMBIE) {
        process_t* current = sched_current();
        if (allow_detach && s_detach_requested == proc) {
            s_detach_requested = 0;
            process_set_foreground(0);
            s_detach_allowed = 0;
            if (current && current->state == PROCESS_STATE_WAITING) {
                current->state = PROCESS_STATE_RUNNING;
            }
            if (detached) {
                *detached = 1;
            }
            return 0;
        }
        if (current) current->state = PROCESS_STATE_WAITING;
        __asm__ __volatile__("sti; hlt");
    }

    process_set_foreground(0);
    s_detach_allowed = 0;
    int status = proc->exit_status;
    process_destroy(proc);
    return status;
}

int process_wait(process_t* proc) {
    return process_wait_impl(proc, 0, 0);
}

int process_wait_detachable(process_t* proc, int* detached) {
    return process_wait_impl(proc, 1, detached);
}

int process_wait_restore_foreground(process_t* proc, process_t* restore_proc) {
    int status;

    if (!proc) return -1;

    s_detach_allowed = 0;
    /*
     * Match process_wait_impl(): callers may already have assigned foreground
     * ownership before entering the blocking wait.
     */
    if (process_get_foreground() == proc) {
        process_set_foreground_preserve_input(proc);
    } else {
        process_set_foreground(proc);
    }
    process_claim_for_wait(proc);

    while (proc->state != PROCESS_STATE_ZOMBIE) {
        process_t* current = sched_current();
        if (current) current->state = PROCESS_STATE_WAITING;
        __asm__ __volatile__("sti; hlt");
    }

    process_set_foreground_preserve_input(restore_proc);
    status = proc->exit_status;
    process_destroy(proc);
    return status;
}

/* ------------------------------------------------------------------ */
/* Reaper task                                                         */
/* ------------------------------------------------------------------ */

/*
 * reaper_task_main — runs as a permanent kernel task.
 *
 * On every wakeup it calls sched_reap_zombies() to destroy any processes
 * that exited without an explicit waiter (e.g. background launches or SYS_EXEC
 * children).  After each scan it halts until the next timer interrupt
 * wakes it, keeping CPU overhead near zero.
 */
static void reaper_task_main(void) {
    for (;;) {
        process_t* current;

        sched_reap_zombies();
        current = sched_current();
        if (current) {
            current->sleep_until = timer_get_ticks() + SMALLOS_TIMER_HZ;
            current->state = PROCESS_STATE_SLEEPING;
        }
        __asm__ __volatile__("sti; hlt");
    }
}

int process_start_reaper(void) {
    process_t* reaper = process_create_kernel_task("reaper", reaper_task_main);
    if (!reaper) {
        terminal_puts("process: failed to create reaper task\n");
        return 0;
    }
    if (!sched_enqueue(reaper)) {
        terminal_puts("process: failed to enqueue reaper task\n");
        process_destroy(reaper);
        return 0;
    }
    return 1;
}
