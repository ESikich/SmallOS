#include "input.h"
#include "process.h"
#include "timer.h"
#include "klib.h"

#define INPUT_QUEUE_SIZE 64u

static sys_input_event_t s_queue[INPUT_QUEUE_SIZE];
static unsigned int s_head = 0;
static unsigned int s_tail = 0;
static unsigned int s_count = 0;
static unsigned int s_sequence = 0;
static void* s_waiting_proc = 0;

static unsigned int irq_save(void) {
    unsigned int flags;

    __asm__ __volatile__(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory");
    return flags;
}

static void irq_restore(unsigned int flags) {
    __asm__ __volatile__(
        "pushl %0\n\t"
        "popfl"
        :
        : "r"(flags)
        : "memory", "cc");
}

static void input_wake_waiter(void) {
    process_t* proc = (process_t*)s_waiting_proc;

    if (proc && proc->state == PROCESS_STATE_WAITING) {
        proc->state = PROCESS_STATE_RUNNING;
    }
    s_waiting_proc = 0;
}

static void input_push_event(const sys_input_event_t* ev) {
    if (!ev) return;

    if (s_count >= INPUT_QUEUE_SIZE) {
        s_tail = (s_tail + 1u) % INPUT_QUEUE_SIZE;
        s_count--;
    }

    s_queue[s_head] = *ev;
    s_head = (s_head + 1u) % INPUT_QUEUE_SIZE;
    s_count++;
    input_wake_waiter();
}

void input_init(void) {
    unsigned int flags = irq_save();

    k_memset(s_queue, 0, sizeof(s_queue));
    s_head = 0;
    s_tail = 0;
    s_count = 0;
    s_sequence = 0;
    s_waiting_proc = 0;

    irq_restore(flags);
}

void input_clear_events(void) {
    unsigned int flags = irq_save();

    s_head = 0;
    s_tail = 0;
    s_count = 0;

    irq_restore(flags);
}

void input_push_key_event(key_event_t ev) {
    sys_input_event_t out;

    if (ev.key == KEY_NONE) return;

    k_memset(&out, 0, sizeof(out));
    out.type = SYS_INPUT_TYPE_KEY;
    out.ticks = timer_get_ticks();
    out.sequence = ++s_sequence;
    out.key = (unsigned int)ev.key;
    out.ascii = (unsigned int)(unsigned char)ev.ascii;
    if (ev.pressed) out.flags |= SYS_INPUT_KEY_PRESSED;
    if (ev.shift) out.flags |= SYS_INPUT_KEY_SHIFT;
    if (ev.ctrl) out.flags |= SYS_INPUT_KEY_CTRL;
    if (ev.alt) out.flags |= SYS_INPUT_KEY_ALT;
    if (ev.caps_lock) out.flags |= SYS_INPUT_KEY_CAPS_LOCK;
    if (ev.num_lock) out.flags |= SYS_INPUT_KEY_NUM_LOCK;
    if (ev.scroll_lock) out.flags |= SYS_INPUT_KEY_SCROLL_LOCK;

    input_push_event(&out);
}

void input_push_mouse_event(int dx, int dy,
                            unsigned int buttons,
                            unsigned int button_changes) {
    sys_input_event_t out;

    if (dx == 0 && dy == 0 && button_changes == 0) return;

    k_memset(&out, 0, sizeof(out));
    out.type = SYS_INPUT_TYPE_MOUSE;
    out.ticks = timer_get_ticks();
    out.sequence = ++s_sequence;
    out.dx = dx;
    out.dy = dy;
    out.buttons = buttons;
    out.button_changes = button_changes;

    input_push_event(&out);
}

int input_available(void) {
    return (int)s_count;
}

int input_pop_event(sys_input_event_t* out) {
    unsigned int flags;

    if (!out) return 0;

    flags = irq_save();
    if (s_count == 0) {
        irq_restore(flags);
        return 0;
    }

    *out = s_queue[s_tail];
    s_tail = (s_tail + 1u) % INPUT_QUEUE_SIZE;
    s_count--;
    irq_restore(flags);
    return 1;
}

void input_set_waiting_process(void* proc) {
    s_waiting_proc = proc;
}

void* input_get_waiting_process(void) {
    return s_waiting_proc;
}

void input_forget_waiting_process(void* proc) {
    if (s_waiting_proc == proc) {
        s_waiting_proc = 0;
    }
}
