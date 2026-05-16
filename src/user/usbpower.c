#include "diag_util.h"

void _start(int argc, char** argv) {
    int powered;

    (void)argc;
    (void)argv;

    u_puts("usbpower: OHCI root-hub power only\n");
    powered = sys_usb_diag_op(SYS_USB_DIAG_OP_POWER, 0);
    if (powered < 0) {
        u_puts("usbpower: failed\n");
        sys_exit(1);
    }
    u_puts("usbpower: total_powered=");
    u_put_uint((unsigned int)powered);
    u_putc('\n');
    sys_exit(0);
}
