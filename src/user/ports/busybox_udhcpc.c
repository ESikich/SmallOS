#include "user_syscall.h"
#include "string.h"
#include "unistd.h"

static int streq(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static void say(const char* text) {
    unsigned int len = 0;

    while (text[len]) {
        len++;
    }
    (void)write(STDERR_FILENO, text, len);
}

static const char* option_value(int argc, char** argv, const char* short_opt) {
    for (int i = 1; i < argc; i++) {
        if (streq(argv[i], short_opt)) {
            return i + 1 < argc ? argv[i + 1] : 0;
        }
    }
    return 0;
}

int __wrap_udhcpc_main(int argc, char** argv) {
    const char* interface = option_value(argc, argv, "-i");
    sys_net_op_request_t req;

    if (!interface) {
        interface = "eth0";
    }
    if (!streq(interface, "eth0")) {
        say("udhcpc: SmallOS supports eth0 only\n");
        return 1;
    }

    memset(&req, 0, sizeof(req));
    req.op = SYS_NET_OP_DHCP;
    if (sys_net_op(&req) > 0) {
        say("udhcpc: lease acquired via SmallOS DHCP\n");
        return 0;
    }

    say("udhcpc: no lease\n");
    return 1;
}
