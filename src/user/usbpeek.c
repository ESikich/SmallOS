#include "diag_util.h"

void _start(int argc, char** argv) {
    unsigned int port;
    int rc;

    if (argc < 2 || !diag_parse_uint(argv[1], &port)) {
        u_puts("usbpeek usage: usbpeek <ohci-port>\n");
        sys_exit(1);
    }

    rc = sys_usb_diag_op(SYS_USB_DIAG_OP_PEEK, port);
    sys_exit(rc < 0 ? 1 : 0);
}
