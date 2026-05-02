#include "process.h"
#include "pmm.h"
#include "paging.h"
#include "klib.h"
#include "fat16.h"
#include "terminal.h"
#include "scheduler.h"
#include "keyboard.h"
#include "shell.h"
#include "../drivers/tcp.h"
#include "uapi_poll.h"

#define FREAD_CACHE_MAX_BYTES (PROCESS_FD_CACHE_PAGES * 4096u)
#define FWRITE_CACHE_MAX_BYTES (PROCESS_FD_CACHE_PAGES * 4096u)

static u8 s_write_flush_buf[FWRITE_CACHE_MAX_BYTES];

/* ------------------------------------------------------------------ */
/* Internal helpers                                                   */
/* ------------------------------------------------------------------ */

static process_t* s_foreground = 0;

/*
 * process_key_consumer — keyboard consumer active while a user process
 * holds the foreground.  Only ASCII characters are meaningful to user
 * programs; non-ASCII key events (arrows, function keys, etc.) are
 * silently ignored.
 *
 * True-blocking SYS_READ wake-up:
 *   After pushing the character into kb_buf, check whether a process is
 *   parked in PROCESS_STATE_WAITING.  If so, mark it PROCESS_STATE_RUNNING
 *   and clear the waiting slot so the scheduler will pick it up on the
 *   next pass.
 *
 *   This runs entirely in IRQ1 context (IF=0).  The only work done is a
 *   state-flag write and a pointer clear — no allocation, no stack switch,
 *   no blocking.
 */
static void process_key_consumer(key_event_t ev) {
    if (!ev.ascii) return;

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

static int process_handle_console_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_console_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_console_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_console_poll(fd_entry_t* ent, short events);
static void process_handle_console_close(fd_entry_t* ent);

void process_fd_cache_free(fd_entry_t* ent) {
    if (!ent) return;

    ent->writable = 0;
    for (unsigned int i = 0; i < ent->cache_page_count; i++) {
        if (ent->cache_pages[i]) {
            pmm_free_frame(ent->cache_pages[i]);
            ent->cache_pages[i] = 0;
        }
    }
    ent->cache_page_count = 0;
}

static int process_handle_file_flush(fd_entry_t* ent);
static int process_fd_seek_file(fd_entry_t* ent, int offset, int whence);
static int process_handle_file_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_file_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_file_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_file_poll(fd_entry_t* ent, short events);
static void process_handle_file_close(fd_entry_t* ent);
static int process_handle_socket_read(fd_entry_t* ent, char* buf, unsigned int len);
static int process_handle_socket_write(fd_entry_t* ent, const char* buf, unsigned int len);
static int process_handle_socket_seek(fd_entry_t* ent, int offset, int whence);
static short process_handle_socket_poll(fd_entry_t* ent, short events);
static void process_handle_socket_close(fd_entry_t* ent);

static const process_handle_ops_t s_file_handle_ops = {
    .read = process_handle_file_read,
    .write = process_handle_file_write,
    .seek = process_handle_file_seek,
    .poll = process_handle_file_poll,
    .flush = process_handle_file_flush,
    .close = process_handle_file_close,
};

static const process_handle_ops_t s_socket_handle_ops = {
    .read = process_handle_socket_read,
    .write = process_handle_socket_write,
    .seek = process_handle_socket_seek,
    .poll = process_handle_socket_poll,
    .flush = 0,
    .close = process_handle_socket_close,
};

static const process_handle_ops_t s_console_handle_ops = {
    .read = process_handle_console_read,
    .write = process_handle_console_write,
    .seek = process_handle_console_seek,
    .poll = process_handle_console_poll,
    .flush = 0,
    .close = process_handle_console_close,
};

static void process_init_standard_fds(process_t* proc) {
    if (!proc) return;

    proc->fds[0].valid = 1;
    proc->fds[0].kind = PROCESS_HANDLE_KIND_CONSOLE;
    proc->fds[0].ops = &s_console_handle_ops;
    proc->fds[0].writable = 0;

    for (int fd = 1; fd <= 2; fd++) {
        proc->fds[fd].valid = 1;
        proc->fds[fd].kind = PROCESS_HANDLE_KIND_CONSOLE;
        proc->fds[fd].ops = &s_console_handle_ops;
        proc->fds[fd].writable = 1;
    }
}

fd_entry_t* process_fd_get(process_t* proc, int fd) {
    if (!proc) return 0;
    if (fd < 0 || fd >= PROCESS_FD_MAX) return 0;
    if (!proc->fds[fd].valid) return 0;
    return &proc->fds[fd];
}

int process_fd_open_file(process_t* proc, const char* name, u32 size, int writable) {
    if (!proc || !name) return -1;

    for (int fd = PROCESS_FD_FIRST; fd < PROCESS_FD_MAX; fd++) {
        if (!proc->fds[fd].valid) {
            fd_entry_t* ent = &proc->fds[fd];
            k_memset(ent, 0, sizeof(*ent));
            ent->valid = 1;
            ent->kind = PROCESS_HANDLE_KIND_FILE;
            ent->ops = &s_file_handle_ops;
            ent->writable = writable ? 1 : 0;
            ent->size = size;
            ent->offset = 0;
            ent->cache_page_count = 0;
            k_memcpy(ent->name, name, (k_size_t)k_strlen(name) + 1u);
            return fd;
        }
    }

    return -1;
}

int process_fd_open_socket(process_t* proc, const char* name) {
    if (!proc) return -1;

    for (int fd = PROCESS_FD_FIRST; fd < PROCESS_FD_MAX; fd++) {
        if (!proc->fds[fd].valid) {
            fd_entry_t* ent = &proc->fds[fd];
            k_memset(ent, 0, sizeof(*ent));
            ent->valid = 1;
            ent->kind = PROCESS_HANDLE_KIND_SOCKET;
            ent->ops = &s_socket_handle_ops;
            ent->socket_state = PROCESS_SOCKET_STATE_OPEN;
            ent->socket_port = 0;
            ent->writable = 0;
            if (name) {
                k_memcpy(ent->name, name, (k_size_t)k_strlen(name) + 1u);
            }
            return fd;
        }
    }

    return -1;
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

    process_fd_cache_free(ent);
    k_memset(ent, 0, sizeof(*ent));
}

static int process_fd_ensure_capacity(fd_entry_t* ent, unsigned int bytes) {
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

static int process_fd_load_cache(fd_entry_t* ent) {
    if (!ent) return 0;
    if (ent->cache_page_count != 0) return 1;
    if (ent->size == 0) return 1;
    if (ent->size > FREAD_CACHE_MAX_BYTES) return 0;

    u32 loaded_size = 0;
    const u8* data = fat16_load(ent->name, &loaded_size);
    if (!data) return 0;
    if (loaded_size != ent->size) return 0;

    u32 pages = (ent->size + 4095u) / 4096u;
    ent->cache_page_count = 0;
    for (u32 i = 0; i < pages; i++) {
        u32 frame = pmm_alloc_frame();
        if (!frame) {
            process_fd_cache_free(ent);
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

static int process_handle_file_flush(fd_entry_t* ent) {
    if (!ent || !ent->valid || !ent->writable) {
        return 0;
    }
    if (ent->size > FWRITE_CACHE_MAX_BYTES) {
        return 0;
    }

    for (u32 i = 0; i < ent->size; i++) {
        u32 page_idx = i / 4096u;
        u32 page_off = i % 4096u;
        s_write_flush_buf[i] = ((u8*)ent->cache_pages[page_idx])[page_off];
    }

    return fat16_write_path(ent->name, s_write_flush_buf, ent->size);
}

static int process_handle_file_read(fd_entry_t* ent, char* buf, unsigned int len) {
    return process_fd_read_file(ent, buf, len);
}

static int process_handle_file_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    return process_fd_write_file(ent, buf, len);
}

static int process_handle_file_seek(fd_entry_t* ent, int offset, int whence) {
    return process_fd_seek_file(ent, offset, whence);
}

static short process_handle_file_poll(fd_entry_t* ent, short events) {
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

static void process_handle_file_close(fd_entry_t* ent) {
    if (!ent) return;
    process_fd_cache_free(ent);
    k_memset(ent, 0, sizeof(*ent));
}

static int process_handle_socket_read(fd_entry_t* ent, char* buf, unsigned int len) {
    if (!ent || !ent->valid) return -1;
    if (ent->socket_state != PROCESS_SOCKET_STATE_CONNECTED) return -1;
    if (len == 0) return 0;

    tcp_socket_use_port(ent->socket_port);
    __asm__ __volatile__("sti");
    while (!tcp_socket_recv_ready()) {
        if (!tcp_socket_connection_established()) {
            __asm__ __volatile__("cli");
            return 0;
        }
        __asm__ __volatile__("hlt");
    }
    __asm__ __volatile__("cli");
    return tcp_socket_recv(buf, len);
}

static int process_handle_socket_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    if (!ent || !ent->valid) return -1;
    if (ent->socket_state != PROCESS_SOCKET_STATE_CONNECTED) return -1;
    if (len == 0) return 0;

    tcp_socket_use_port(ent->socket_port);
    return tcp_socket_send(buf, len);
}

static int process_handle_socket_seek(fd_entry_t* ent, int offset, int whence) {
    (void)ent;
    (void)offset;
    (void)whence;
    return -1;
}

static short process_handle_socket_poll(fd_entry_t* ent, short events) {
    short revents = 0;

    if (!ent || !ent->valid) return POLLERR;
    tcp_socket_use_port(ent->socket_port);

    if (ent->socket_state == PROCESS_SOCKET_STATE_LISTENER) {
        if ((events & POLLIN) && tcp_socket_accept_ready()) {
            revents |= POLLIN;
        }
    } else if (ent->socket_state == PROCESS_SOCKET_STATE_CONNECTED) {
        if ((events & POLLIN) && tcp_socket_recv_ready()) {
            revents |= POLLIN;
        }
        if ((events & POLLOUT) && tcp_socket_connection_established()) {
            revents |= POLLOUT;
        }
    }

    return revents;
}

static int process_handle_console_read(fd_entry_t* ent, char* buf, unsigned int len) {
    process_t* proc = sched_current();
    unsigned int n = 0;

    if (!ent || !ent->valid || ent->writable) return -1;
    if (!buf) return -1;
    if (len == 0) return 0;

    __asm__ volatile ("sti");

    while (n < len) {
        while (!keyboard_buf_available()) {
            if (proc) {
                proc->state = PROCESS_STATE_WAITING;
                keyboard_set_waiting_process(proc);
            }
            __asm__ volatile ("hlt");
        }

        char c = keyboard_buf_pop();
        terminal_putc(c);
        buf[n++] = c;
        if (c == '\n') break;
    }

    __asm__ volatile ("cli");
    return (int)n;
}

static int process_handle_console_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    if (!ent || !ent->valid || !ent->writable) return -1;
    if (!buf) return -1;

    for (unsigned int i = 0; i < len; i++) {
        terminal_putc(buf[i]);
    }
    return (int)len;
}

static int process_handle_console_seek(fd_entry_t* ent, int offset, int whence) {
    (void)ent;
    (void)offset;
    (void)whence;
    return -1;
}

static short process_handle_console_poll(fd_entry_t* ent, short events) {
    short revents = 0;

    if (!ent || !ent->valid) return POLLERR;
    if (!ent->writable && (events & POLLIN) && keyboard_buf_available()) {
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

static void process_handle_socket_close(fd_entry_t* ent) {
    if (!ent) return;
    tcp_socket_handle_close(ent);
    k_memset(ent, 0, sizeof(*ent));
}

int process_fd_read(fd_entry_t* ent, char* buf, unsigned int len) {
    if (!ent || !ent->ops || !ent->ops->read) return -1;
    return ent->ops->read(ent, buf, len);
}

int process_fd_write(fd_entry_t* ent, const char* buf, unsigned int len) {
    if (!ent || !ent->ops || !ent->ops->write) return -1;
    return ent->ops->write(ent, buf, len);
}

short process_fd_poll(fd_entry_t* ent, short events) {
    if (!ent || !ent->ops || !ent->ops->poll) return POLLERR;
    return ent->ops->poll(ent, events);
}

int process_fd_seek(fd_entry_t* ent, int offset, int whence) {
    if (!ent || !ent->ops || !ent->ops->seek) return -1;
    return ent->ops->seek(ent, offset, whence);
}

int process_fd_flush(fd_entry_t* ent) {
    if (!ent || !ent->ops || !ent->ops->flush) return 0;
    return ent->ops->flush(ent);
}

int process_fd_write_file(fd_entry_t* ent, const char* buf, unsigned int len) {
    if (!ent || !ent->valid || !ent->writable) return -1;
    if (len == 0) return 0;

    unsigned int end = ent->offset + len;
    if (end < ent->offset) return -1;
    if (!process_fd_ensure_capacity(ent, end)) return -1;

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

static int process_fd_seek_file(fd_entry_t* ent, int offset, int whence) {
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

int process_fd_read_file(fd_entry_t* ent, char* buf, unsigned int len) {
    unsigned int remaining;
    unsigned int to_copy;
    unsigned int src_off;

    if (!ent || !ent->valid) return -1;
    if (len == 0) return 0;
    if (ent->offset >= ent->size) return 0;

    if (!process_fd_load_cache(ent)) return -1;

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

void process_claim_for_wait(process_t* proc) {
    if (!proc) return;
    proc->reaper_claimed = 1;
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
    if (!frame) {
        terminal_puts("process: out of frames for process_t\n");
        return 0;
    }

    process_t* proc = (process_t*)frame;
    proc_zero(proc);

    proc->state = PROCESS_STATE_UNUSED;
    proc->heap_base = USER_HEAP_BASE;
    proc->heap_brk = USER_HEAP_BASE;
    process_init_standard_fds(proc);
    if (name) {
        str_copy_n(proc->name, name, PROCESS_NAME_MAX);
    }

    return proc;
}

process_t* process_create_kernel_task(const char* name, void (*entry)(void)) {
    process_t* proc = process_create(name);
    if (!proc) return 0;

    proc->kernel_stack_frame = pmm_alloc_frame();
    if (!proc->kernel_stack_frame) {
        terminal_puts("process: out of frames for kernel stack\n");
        process_destroy(proc);
        return 0;
    }

    proc->pd = 0;
    proc->kernel_entry = entry;
    proc->state = PROCESS_STATE_RUNNING;

    {
        unsigned int* stack_top = (unsigned int*)(proc->kernel_stack_frame + 4096u);
        stack_top--;
        *stack_top = (unsigned int)process_kernel_task_bootstrap;
        proc->sched_esp = (unsigned int)stack_top;
    }

    return proc;
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

    for (int i = 0; i < PROCESS_FD_MAX; i++) {
        if (proc->fds[i].valid) {
            process_fd_close(&proc->fds[i]);
        } else {
            process_fd_cache_free(&proc->fds[i]);
        }
    }

    if (proc->pd) {
        process_pd_destroy(proc->pd);
        proc->pd = 0;
    }

    if (proc->kernel_stack_frame) {
        pmm_free_frame(proc->kernel_stack_frame);
        proc->kernel_stack_frame = 0;
    }

    if (s_foreground == proc) {
        s_foreground = 0;
    }

    proc->state = PROCESS_STATE_EXITED;
    pmm_free_frame((u32)proc);
}

process_t* process_get_current(void) {
    return sched_current();
}

void process_set_foreground(process_t* proc) {
    s_foreground = proc;
    if (proc) {
        keyboard_buf_clear();           /* discard any input that arrived before
                                           the process was ready to read it,
                                           e.g. the Enter that launched runelf */
        keyboard_set_consumer(process_key_consumer);
    } else {
        shell_register_consumer();
    }
}

process_t* process_get_foreground(void) {
    return s_foreground;
}

int process_wait(process_t* proc) {
    if (!proc) return -1;

    process_set_foreground(proc);
    process_claim_for_wait(proc);

    while (proc->state != PROCESS_STATE_ZOMBIE) {
        __asm__ __volatile__("sti; hlt");
    }

    process_set_foreground(0);
    int status = proc->exit_status;
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
 * that exited without an explicit waiter (e.g. runelf_nowait or SYS_EXEC
 * children).  After each scan it halts until the next timer interrupt
 * wakes it, keeping CPU overhead near zero.
 */
static void reaper_task_main(void) {
    for (;;) {
        sched_reap_zombies();
        __asm__ __volatile__("sti; hlt");
    }
}

void process_start_reaper(void) {
    process_t* reaper = process_create_kernel_task("reaper", reaper_task_main);
    if (!reaper) {
        terminal_puts("process: failed to create reaper task\n");
        return;
    }
    if (!sched_enqueue(reaper)) {
        terminal_puts("process: failed to enqueue reaper task\n");
        process_destroy(reaper);
    }
}
