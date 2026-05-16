#include "diag_util.h"

void _start(int argc, char** argv) {
    sys_net_op_request_t req;

    (void)argc;
    (void)argv;

    memset(&req, 0, sizeof(req));
    req.op = SYS_NET_OP_POLL_ONCE;
    if (sys_net_op(&req) > 0) {
        u_puts("netrecv: dispatched packet\n");
        sys_exit(0);
    }

    u_puts("netrecv: no packet\n");
    sys_exit(1);
}
