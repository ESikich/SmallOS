#include "syscall.h"
#include "terminal.h"
#include "timer.h"
#include "paging.h"
#include "keyboard.h"
#include "scheduler.h"
#include "process.h"
#include "pmm.h"
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

/*
 * User virtual address space: [USER_CODE_BASE, USER_STACK_TOP)
 *
 *   0x400000   USER_CODE_BASE  — lowest mapped user address (ELF load base)
 *   0xC0000000 USER_STACK_TOP  — top of user virtual space (exclusive)
 *
 * user_buf_ok(ptr, len) returns 1 if the byte range [ptr, ptr+len) lies
 * entirely within user space, 0 otherwise.
 *
 * Rules:
 *   - ptr must be non-null and >= USER_CODE_BASE
 *   - ptr+len must be <= USER_STACK_TOP (checked without overflow)
 *   - len must be > 0 (callers handle len==0 before calling)
 *
 * This is an address-range check only — it does not walk page tables.
 * A user process could pass a valid-range address that is not backed by a
 * present PTE, which would page-fault in the kernel.  That fault path is
 * not handled today; this check closes the easier attack of passing an
 * explicit kernel address to read or corrupt kernel memory.
 */
static int user_buf_ok(unsigned int ptr, unsigned int len) {
    if (ptr < USER_CODE_BASE)       return 0;   /* catches null and kernel addrs */
    if (len == 0)                   return 0;
    /* Overflow-safe upper bound check: */
    if (len > USER_STACK_TOP - ptr) return 0;
    return 1;
}

/*
 * user_str_ok(ptr) — validate a user-supplied string pointer base.
 *
 * Only checks that the pointer itself falls inside user space.  The full
 * string length is unknown before scanning, so we cannot do a complete
 * range check.  Callers that copy the string (sys_exec_impl) bound the
 * copy explicitly and stop at '\0', so a base check is sufficient to
 * block kernel-address attacks.
 */
static int user_str_ok(unsigned int ptr) {
    if (ptr < USER_CODE_BASE)  return 0;
    if (ptr >= USER_STACK_TOP) return 0;
    return 1;
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
 *      pointer array itself is within user space.
 *   2. Each argv[i] string pointer is checked with user_str_ok() before
 *      elf_seed_sched_context() calls k_strlen/k_memcpy on it.  Without
 *      this, a user process could pass a kernel-address string pointer
 *      that would dereference silently in ring-0 during the copy.
 *   3. The checks happen here, while the caller's CR3 is still active,
 *      so the argv array and its strings are readable.
 *
 * Returns 0 on success, -1 if any validation fails or the program was
 * not found.
 */
static int sys_exec_impl(const char* name, int argc, char** argv) {
    if (!user_str_ok((unsigned int)name)) return -1;

    if (argc < 0 || argc > PROCESS_MAX_ARGS) return -1;

    /* Validate the argv pointer array itself */
    if (argc > 0 && !user_buf_ok((unsigned int)argv,
                                  (unsigned int)argc * sizeof(char*))) {
        return -1;
    }

    /* Validate each individual argv[i] string pointer */
    for (int i = 0; i < argc; i++) {
        if (!user_str_ok((unsigned int)argv[i])) return -1;
    }

    char kname[EXEC_NAME_MAX];
    unsigned int i = 0;
    while (i < EXEC_NAME_MAX - 1 && name[i] != '\0') {
        kname[i] = name[i];
        i++;
    }
    kname[i] = '\0';

    if (i == 0) return -1;

    return elf_run_named(kname, argc, argv) ? 0 : -1;
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
    if (!user_str_ok((unsigned int)name)) return -1;

    /* Copy name into kernel buffer — bounded by PROCESS_FD_NAME_MAX */
    char kname[PROCESS_FD_NAME_MAX];
    unsigned int i = 0;
    while (i < PROCESS_FD_NAME_MAX - 1 && name[i] != '\0') {
        kname[i] = name[i];
        i++;
    }
    kname[i] = '\0';
    if (i == 0) return -1;

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
            for (unsigned int j = 0; j <= i; j++)
                proc->fds[fd].name[j] = kname[j];
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
