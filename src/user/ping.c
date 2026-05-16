#include "diag_util.h"

void _start(int argc, char** argv) {
    sys_net_op_request_t req;
    int rc;

    memset(&req, 0, sizeof(req));
    req.op = SYS_NET_OP_PING;
    if (argc >= 2 && !diag_parse_ip(argv[1], &req.target_ip)) {
        u_puts("ping: invalid ip\n");
        sys_exit(1);
    }

    rc = sys_net_op(&req);
    if (rc < 0) {
        u_puts("ping: IPv4 gateway is not configured\n");
        sys_exit(1);
    }

    u_puts("ping: ");
    diag_put_ip(req.target_ip);
    u_puts(" from ");
    diag_put_ip(req.sender_ip);
    u_putc('\n');
    u_puts(rc > 0 ? "ping: ok\n" : "ping: failed\n");
    sys_exit(rc > 0 ? 0 : 1);
}
