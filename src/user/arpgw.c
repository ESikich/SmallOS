#include "diag_util.h"

void _start(int argc, char** argv) {
    sys_net_op_request_t req;
    int rc;

    (void)argc;
    (void)argv;

    memset(&req, 0, sizeof(req));
    req.op = SYS_NET_OP_ARP;
    rc = sys_net_op(&req);
    if (rc < 0) {
        u_puts("arpgw: IPv4 gateway is not configured\n");
        sys_exit(1);
    }

    u_puts("arpgw: who-has ");
    diag_put_ip(req.next_hop_ip);
    u_puts(" from ");
    diag_put_ip(req.sender_ip);
    u_putc('\n');

    if (rc == 0) {
        u_puts("arpgw: no reply\n");
        sys_exit(1);
    }

    u_puts("arpgw: ");
    diag_put_ip(req.next_hop_ip);
    u_puts(" is-at ");
    diag_put_mac(req.mac);
    u_putc('\n');
    sys_exit(0);
}
