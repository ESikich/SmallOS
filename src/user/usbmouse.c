#include "diag_util.h"

void _start(int argc, char** argv) {
    unsigned int port = 0u;
    unsigned int seconds = 3u;
    unsigned int deadline;
    unsigned int events = 0u;
    int opened;

    if (argc >= 2 && !diag_parse_uint(argv[1], &port)) {
        u_puts("usbmouse usage: usbmouse [port] [seconds]\n");
        sys_exit(1);
    }
    if (argc >= 3 && !diag_parse_uint(argv[2], &seconds)) {
        u_puts("usbmouse usage: usbmouse [port] [seconds]\n");
        sys_exit(1);
    }

    u_puts("usbmouse: command start seconds=");
    u_put_uint(seconds);
    u_puts(" port=");
    u_put_uint(port);
    u_putc('\n');

    opened = sys_usb_mouse_op(SYS_USB_MOUSE_OP_OPEN, port);
    if (opened <= 0) {
        u_puts("usbmouse: no OHCI boot mouse found\n");
        sys_exit(opened < 0 ? 1 : 0);
    }

    deadline = sys_get_ticks() + seconds * 100u;
    u_puts("usbmouse: move/click mouse\n");
    while ((int)(sys_get_ticks() - deadline) < 0) {
        int poll = sys_usb_mouse_op(SYS_USB_MOUSE_OP_POLL, 0);
        if (poll < 0) break;
        if (poll > 0) events++;
        sys_yield();
    }

    u_puts("usbmouse: events=");
    u_put_uint(events);
    u_putc('\n');
    (void)sys_usb_mouse_op(SYS_USB_MOUSE_OP_CLOSE, 0);
    sys_exit(0);
}
