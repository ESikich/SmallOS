#include "syscall_internal.h"
#include "process.h"
#include "scheduler.h"
#include "timer.h"
#include "uapi_errno.h"
#include "uapi_time.h"
#include "../drivers/ntp.h"

unsigned int sys_get_ticks_impl(void) {
    return timer_get_ticks();
}

void sys_wait_until_current_running(process_t* proc) {
    __asm__ volatile ("sti");
    while (proc && proc->state != PROCESS_STATE_RUNNING) {
        __asm__ volatile ("hlt");
    }
    __asm__ volatile ("cli");
}

int sys_yield_impl(void) {
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
int sys_sleep_impl(syscall_regs_t* regs, unsigned int ticks) {
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

int sys_clock_gettime_impl(int clock_id, struct user_timespec* ts) {
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

int sys_clock_settime_impl(int clock_id, const struct user_timespec* ts) {
    struct user_timespec in;

    if (!ts) return -EFAULT;
    if (clock_id != CLOCK_REALTIME) return -EINVAL;
    if (copy_from_user(&in, ts, sizeof(in)) < 0) return -EFAULT;
    if (in.tv_nsec < 0 || in.tv_nsec >= (long)SMALLOS_NS_PER_SECOND) return -EINVAL;

    timer_set_realtime_seconds(in.tv_sec);
    return 0;
}

int sys_ntp_sync_impl(unsigned int server_ip, struct user_timespec* out_ts) {
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

