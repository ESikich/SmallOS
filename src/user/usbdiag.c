#include "diag_util.h"

void _start(int argc, char** argv) {
    sys_usb_port_snapshot_t snap;
    int rc;

    (void)argc;
    (void)argv;

    u_puts("usbdiag: begin\n");
    rc = sys_usb_port_snapshot(&snap);
    if (rc == 0) {
        diag_usb_print_ports(&snap);
        diag_usb_print_dry_run_candidates(&snap);
    }
    u_puts("usbdiag: done\n");
    sys_exit(rc < 0 ? 1 : 0);
}
