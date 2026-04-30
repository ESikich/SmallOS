#include "syscall.h"
#include "terminal.h"
#include "timer.h"
#include "paging.h"
#include "keyboard.h"
#include "scheduler.h"
#include "process.h"
#include "pmm.h"
#include "klib.h"
#include "../exec/elf_loader.h"
#include "fat16.h"

#define SYSCALL_ERR_INVALID   ((unsigned int)-1)
#define SYSCALL_MAX_WRITE_LEN 4096u
#define EXEC_NAME_MAX         32
#define SCHED_RESUME_RETADDR_OFFSET 8u  /* push esp + call return address */
#define FREAD_CACHE_MAX_BYTES (PROCESS_FD_CACHE_PAGES * 4096u)

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
    u32 pde = pd[addr >> 22];
    if (!(pde & PAGE_PRESENT)) return 0;
    if (!(pde & PAGE_USER))    return 0;

    u32* pt = (u32*)(pde & ~0xFFFu);
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

/*
 * copy_user_cstr(dst, dst_size, src)
 *
 * Copy a NUL-terminated string from user space into a kernel buffer.
 * The copy stops at the first '\0'.  Returns the number of bytes copied,
 * including the terminator, or -1 on validation failure or truncation.
 */
static int copy_user_cstr(char* dst, unsigned int dst_size, const char* src) {
    if (!dst || !src || dst_size == 0) return -1;

    unsigned int ptr = (unsigned int)src;
    if (ptr < USER_CODE_BASE || ptr >= USER_STACK_TOP) return -1;

    u32* pd = current_user_pd();
    if (!pd) return -1;

    for (unsigned int i = 0; i < dst_size; i++) {
        unsigned int addr = ptr + i;
        if (addr < USER_CODE_BASE || addr >= USER_STACK_TOP) return -1;
        if (!user_page_mapped(pd, addr)) return -1;

        dst[i] = src[i];
        if (dst[i] == '\0') {
            return (int)(i + 1);
        }
    }

    return -1;
}

/* ------------------------------------------------------------------ */
/* Syscall implementations                                            */
/* ------------------------------------------------------------------ */

static int sys_write_impl(const char* buf, unsigned int len) {
    if (len == 0) return 0;
    if (len > SYSCALL_MAX_WRITE_LEN) return -1;
    if (!user_buf_ok((unsigned int)buf, len)) return -1;

    for (unsigned int i = 0; i < len; i++) {
        terminal_putc(buf[i]);
    }
    return (int)len;
}

static int sys_putc_impl(unsigned int ch) {
    terminal_putc((char)ch);
    return 1;
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

static int sys_yield_impl(unsigned int esp) {
    sched_yield_now(esp - SCHED_RESUME_RETADDR_OFFSET);
    return 0;
}

/*
 * sys_sleep_impl(regs, ticks)
 *
 * Block the current process until at least ticks timer ticks have elapsed.
 * The task marks itself SLEEPING, stores a wake deadline, then yields to
 * the scheduler.  When the timer reaches the deadline the scheduler wakes
 * the task and this function continues.
 */
static int sys_sleep_impl(syscall_regs_t* regs, unsigned int ticks) {
    if (ticks == 0) return 0;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -1;

    proc->sleep_until = timer_get_ticks() + ticks;
    proc->state = PROCESS_STATE_SLEEPING;

    /*
     * Yield immediately so other runnable tasks can execute while this
     * one sleeps.  If there is no other runnable task, sched_yield_now()
     * simply returns and the local hlt loop keeps the CPU idle.
     */
    __asm__ volatile ("sti");
    sched_yield_now((unsigned int)regs - SCHED_RESUME_RETADDR_OFFSET);

    while (proc->state != PROCESS_STATE_RUNNING) {
        __asm__ volatile ("hlt");
    }

    __asm__ volatile ("cli");
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
    if (!user_buf_ok((unsigned int)buf, len)) return -1;

    process_t* proc = (process_t*)sched_current();

    unsigned int n = 0;

    __asm__ volatile ("sti");

    while (n < len) {
        /* Park until at least one character is available. */
        while (!keyboard_buf_available()) {
            if (proc) {
                proc->state = PROCESS_STATE_WAITING;
                keyboard_set_waiting_process(proc);
            }
            __asm__ volatile ("hlt");
            /*
             * Execution resumes here after the timer IRQ switches back to
             * this task.  proc->state has been set to RUNNING by
             * process_key_consumer() before the scheduler selected us again.
             * Re-check the buffer — if it is still empty we park again.
             */
        }

        char c = keyboard_buf_pop();
        terminal_putc(c);
        buf[n++] = c;
        if (c == '\n') break;
    }

    __asm__ volatile ("cli");
    return (int)n;
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
 * Returns 0 on success, -1 if any validation fails or the program was
 * not found.
 */
static int sys_exec_impl(const char* name, int argc, char** argv) {
    char kname[EXEC_NAME_MAX];
    char kargv_data[PROCESS_ARG_BYTES];
    char* kargv[PROCESS_MAX_ARGS + 1];
    unsigned int used = 0;

    if (copy_user_cstr(kname, sizeof(kname), name) <= 1) return -1;

    if (argc < 0 || argc > PROCESS_MAX_ARGS) return -1;

    /* Validate the argv pointer array itself */
    if (argc > 0 && !user_buf_ok((unsigned int)argv,
                                  (unsigned int)argc * sizeof(char*))) {
        return -1;
    }

    for (int i = 0; i < argc; i++) {
        int copied = copy_user_cstr(&kargv_data[used],
                                    PROCESS_ARG_BYTES - used,
                                    argv[i]);
        if (copied < 0) return -1;
        kargv[i] = &kargv_data[used];
        used += (unsigned int)copied;
    }
    kargv[argc] = 0;

    return elf_run_named(kname, argc, kargv) ? 0 : -1;
}

/*
 * sys_writefile_impl(name, buf, len)
 *
 * Create or overwrite a root-directory FAT16 file in one shot.  This is
 * the simplest output primitive for user-space tools that need to emit a
 * compiler product or generated artifact.
 */
static int sys_writefile_impl(const char* name, const void* buf, unsigned int len) {
    char kname[EXEC_NAME_MAX];
    if (copy_user_cstr(kname, sizeof(kname), name) <= 1) return -1;
    if (len > 0 && !user_buf_ok((unsigned int)buf, len)) return -1;

    return fat16_write(kname, (const u8*)buf, len) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* File descriptor syscalls                                           */
/* ------------------------------------------------------------------ */

/*
 * sys_open_impl(name)
 *
 * Validate the filename, confirm the file exists on the FAT16 partition
 * via fat16_stat(), allocate the lowest free fd slot (>= PROCESS_FD_FIRST)
 * in the current process's fd table, and record name, size, and offset=0.
 *
 * Returns the fd (>= 3) on success, -1 on any failure.
 */
static int sys_open_impl(const char* name) {
    char kname[PROCESS_FD_NAME_MAX];
    if (copy_user_cstr(kname, sizeof(kname), name) <= 1) return -1;

    /* Check the file exists and get its size */
    u32 file_size = 0;
    if (!fat16_stat(kname, &file_size)) return -1;

    /* Allocate an fd slot in the current process */
    process_t* proc = (process_t*)sched_current();
    if (!proc) return -1;

    for (int fd = PROCESS_FD_FIRST; fd < PROCESS_FD_MAX; fd++) {
        if (!proc->fds[fd].valid) {
            proc->fds[fd].valid  = 1;
            proc->fds[fd].size   = file_size;
            proc->fds[fd].offset = 0;
            k_memcpy(proc->fds[fd].name, kname, (k_size_t)k_strlen(kname) + 1u);
            return fd;
        }
    }

    return -1;   /* fd table full */
}

/*
 * sys_close_impl(fd)
 *
 * Mark the fd slot as free.  Returns 0 on success, -1 on bad fd.
 */
static int sys_close_impl(int fd) {
    if (fd < PROCESS_FD_FIRST || fd >= PROCESS_FD_MAX) return -1;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -1;
    if (!proc->fds[fd].valid) return -1;

    process_fd_cache_free(&proc->fds[fd]);
    proc->fds[fd].valid  = 0;
    proc->fds[fd].offset = 0;
    proc->fds[fd].size   = 0;
    proc->fds[fd].name[0] = '\0';
    return 0;
}

static int fd_cache_load(process_t* proc, fd_entry_t* ent) {
    if (!proc || !ent) return 0;
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

/*
 * sys_fread_impl(fd, buf, len)
 *
 * Read up to len bytes from the file at fd into the user buffer buf,
 * starting at the current file offset.  Advances the offset.
 *
 * Implementation:
 *   The first read loads the file into PMM-backed per-fd cache pages.
 *   Later reads reuse that cache until sys_close() or process teardown.
 *
 * Returns bytes copied (0 at EOF), -1 on error.
 */
static int sys_fread_impl(int fd, char* buf, unsigned int len) {
    if (fd < PROCESS_FD_FIRST || fd >= PROCESS_FD_MAX) return -1;
    if (len == 0) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -1;

    process_t* proc = (process_t*)sched_current();
    if (!proc) return -1;
    if (!proc->fds[fd].valid) return -1;

    fd_entry_t* ent = &proc->fds[fd];

    /* Already at or past end of file */
    if (ent->offset >= ent->size) return 0;

    if (!fd_cache_load(proc, ent)) return -1;

    /* Clamp to remaining bytes */
    u32 remaining = ent->size - ent->offset;
    u32 to_copy   = (len < remaining) ? len : remaining;

    /* Copy from kernel buffer into user buffer */
    u32 src_off = ent->offset;
    for (u32 i = 0; i < to_copy; i++) {
        u32 page_idx = (src_off + i) / 4096u;
        u32 page_off = (src_off + i) % 4096u;
        const u8* src = (const u8*)ent->cache_pages[page_idx];
        buf[i] = (char)src[page_off];
    }

    ent->offset += to_copy;
    return (int)to_copy;
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

        case SYS_YIELD:
            regs->eax = (unsigned int)sys_yield_impl((unsigned int)regs);
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

        case SYS_EXEC:
            regs->eax = (unsigned int)sys_exec_impl(
                            (const char*)regs->ebx,
                            (int)regs->ecx,
                            (char**)regs->edx);
            break;

        case SYS_OPEN:
            regs->eax = (unsigned int)sys_open_impl(
                            (const char*)regs->ebx);
            break;

        case SYS_CLOSE:
            regs->eax = (unsigned int)sys_close_impl((int)regs->ebx);
            break;

        case SYS_FREAD:
            regs->eax = (unsigned int)sys_fread_impl(
                            (int)regs->ebx,
                            (char*)regs->ecx,
                            regs->edx);
            break;

        default:
            regs->eax = SYSCALL_ERR_INVALID;
            break;
    }
}
