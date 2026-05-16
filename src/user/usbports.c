#include "diag_util.h"

void _start(int argc, char** argv) {
    int rc;

    (void)argc;
    (void)argv;

    rc = sys_usb_diag_op(SYS_USB_DIAG_OP_PORTS, 0);
    sys_exit(rc < 0 ? 1 : 0);
}
