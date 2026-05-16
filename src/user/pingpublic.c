#include "diag_util.h"

void _start(int argc, char** argv) {
    sys_net_op_request_t req;
    int rc;

    (void)argc;
    (void)argv;

    memset(&req, 0, sizeof(req));
    req.op = SYS_NET_OP_PING;
    req.target_ip = 0x01010101u;
    rc = sys_net_op(&req);
    if (rc < 0) {
        u_puts("pingpublic: IPv4 gateway is not configured\n");
        sys_exit(1);
    }

    u_puts("pingpublic: ");
    diag_put_ip(req.target_ip);
    u_puts(" from ");
    diag_put_ip(req.sender_ip);
    u_putc('\n');
    u_puts(rc > 0 ? "pingpublic: ok\n" : "pingpublic: failed\n");
    u_puts("pingpublic: note: some hypervisors do not forward public ICMP\n");
    u_puts("pingpublic: use pinggw for the supported gateway check\n");
    sys_exit(rc > 0 ? 0 : 1);
}
