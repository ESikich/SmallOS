#include "diag_util.h"

static int run_step(unsigned int op, const char* label, const char* fail) {
    sys_net_op_request_t req;
    int rc;

    memset(&req, 0, sizeof(req));
    req.op = op;
    u_puts("netcheck: ");
    u_puts(label);
    u_putc('\n');
    rc = sys_net_op(&req);
    if (rc <= 0) {
        u_puts(rc < 0 ? "netcheck: IPv4 gateway is not configured\n" : fail);
        return 0;
    }
    u_puts("netcheck: ");
    u_puts(label);
    u_puts(" ok\n");
    return 1;
}

void _start(int argc, char** argv) {
    sys_net_op_request_t req;
    int rc;

    (void)argc;
    (void)argv;

    if (!run_step(SYS_NET_OP_ARP, "gateway arp", "netcheck: gateway arp failed\n")) {
        sys_exit(1);
    }
    if (!run_step(SYS_NET_OP_PING, "gateway ping", "netcheck: gateway ping failed\n")) {
        sys_exit(1);
    }

    memset(&req, 0, sizeof(req));
    req.op = SYS_NET_OP_PING;
    req.target_ip = 0x01010101u;
    u_puts("netcheck: public ping\n");
    rc = sys_net_op(&req);
    if (rc <= 0) {
        u_puts("netcheck: public ping failed\n");
        u_puts("netcheck: note: some hypervisors do not forward public ICMP\n");
        u_puts("netcheck: gateway is ok\n");
        sys_exit(1);
    }

    u_puts("netcheck: public ping ok\n");
    sys_exit(0);
}
