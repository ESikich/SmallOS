#include "diag_util.h"

void _start(int argc, char** argv) {
    sys_usb_port_snapshot_t snap;
    int rc;

    (void)argc;
    (void)argv;

    rc = sys_usb_port_snapshot(&snap);
    if (rc == 0) {
        diag_usb_print_ports(&snap);
    }
    sys_exit(rc < 0 ? 1 : 0);
}
