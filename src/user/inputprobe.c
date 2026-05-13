#include "user_lib.h"
#include "errno.h"

static void fail(const char* msg) {
    u_puts("inputprobe FAIL ");
    u_puts(msg);
    u_putc('\n');
    sys_exit(1);
}

void _start(int argc, char** argv) {
    sys_input_event_t ev;
    int n;

    (void)argc;
    (void)argv;

    u_puts("inputprobe start\n");

    n = sys_input_read((sys_input_event_t*)0x1234, 1u, SYS_INPUT_FLAG_NONBLOCK);
    if (n != -EFAULT) {
        fail("bad pointer");
    }

    n = sys_input_read(&ev, 1u, 0x80000000u);
    if (n != -EINVAL) {
        fail("bad flags");
    }

    n = sys_input_read(&ev, 1u, SYS_INPUT_FLAG_NONBLOCK);
    if (n < 0) {
        fail("nonblock read");
    }

    u_puts("inputprobe waiting for key\n");
    n = sys_input_read(&ev, 1u, 0u);
    if (n != 1) {
        fail("blocking read");
    }
    if (ev.type != SYS_INPUT_TYPE_KEY ||
        (ev.flags & SYS_INPUT_KEY_PRESSED) == 0 ||
        ev.ascii != 'x') {
        fail("key event");
    }

    u_puts("inputprobe key ascii=");
    u_put_uint(ev.ascii);
    u_puts(" seq=");
    u_put_uint(ev.sequence);
    u_putc('\n');

    u_puts("inputprobe PASS\n");
    sys_exit(0);
}
