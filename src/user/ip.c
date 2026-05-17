#include "user_lib.h"

typedef enum {
    SHOW_SUMMARY,
    SHOW_ADDR,
    SHOW_ROUTE,
    SHOW_DNS,
    SHOW_ALL,
} show_mode_t;

static int streq(const char* a, const char* b) {
    unsigned int i = 0;
    while (a[i] && b[i]) {
        if (a[i] != b[i]) return 0;
        i++;
    }
    return a[i] == '\0' && b[i] == '\0';
}

static int starts_with(const char* s, const char* prefix) {
    unsigned int i = 0;
    while (prefix[i]) {
        if (s[i] != prefix[i]) return 0;
        i++;
    }
    return 1;
}

static int contains_char(const char* s, char ch) {
    while (*s) {
        if (*s++ == ch) return 1;
    }
    return 0;
}

static const char* basename(const char* path) {
    const char* base = path;
    while (*path) {
        if (*path == '/') base = path + 1;
        path++;
    }
    return base;
}

static void put_ip(uint32_t ip) {
    u_put_uint((ip >> 24) & 0xFFu);
    u_putc('.');
    u_put_uint((ip >> 16) & 0xFFu);
    u_putc('.');
    u_put_uint((ip >> 8) & 0xFFu);
    u_putc('.');
    u_put_uint(ip & 0xFFu);
}

static void put_mac(const unsigned char* mac) {
    static const char hex[] = "0123456789ABCDEF";
    for (unsigned int i = 0; i < 6u; i++) {
        u_putc(hex[mac[i] >> 4]);
        u_putc(hex[mac[i] & 0x0Fu]);
        if (i != 5u) u_putc(':');
    }
}

static int parse_uint(const char** p, uint32_t max, uint32_t* out) {
    uint32_t value = 0;
    unsigned int digits = 0;

    while (**p >= '0' && **p <= '9') {
        value = value * 10u + (uint32_t)(**p - '0');
        if (value > max) return 0;
        (*p)++;
        digits++;
    }

    if (digits == 0u) return 0;
    *out = value;
    return 1;
}

static int parse_ip(const char* s, uint32_t* out) {
    uint32_t a;
    uint32_t b;
    uint32_t c;
    uint32_t d;
    const char* p = s;

    if (!parse_uint(&p, 255u, &a) || *p++ != '.') return 0;
    if (!parse_uint(&p, 255u, &b) || *p++ != '.') return 0;
    if (!parse_uint(&p, 255u, &c) || *p++ != '.') return 0;
    if (!parse_uint(&p, 255u, &d) || *p != '\0') return 0;

    *out = (a << 24) | (b << 16) | (c << 8) | d;
    return 1;
}

static int prefix_to_mask(uint32_t prefix, uint32_t* out_mask) {
    if (prefix > 32u || !out_mask) return 0;
    if (prefix == 0u) {
        *out_mask = 0u;
        return 1;
    }
    *out_mask = 0xFFFFFFFFu << (32u - prefix);
    return 1;
}

static uint32_t mask_to_prefix(uint32_t mask, int* exact) {
    uint32_t prefix = 0;
    int zero_seen = 0;

    if (exact) *exact = 1;
    for (int bit = 31; bit >= 0; bit--) {
        if (mask & (1u << (unsigned int)bit)) {
            if (zero_seen && exact) *exact = 0;
            prefix++;
        } else {
            zero_seen = 1;
        }
    }
    return prefix;
}

static int parse_ip_prefix(const char* s, uint32_t* out_ip, uint32_t* out_mask) {
    char ip_buf[24];
    unsigned int pos = 0;
    const char* p = s;
    uint32_t prefix;

    while (*p && *p != '/') {
        if (pos + 1u >= sizeof(ip_buf)) return 0;
        ip_buf[pos++] = *p++;
    }
    ip_buf[pos] = '\0';

    if (!parse_ip(ip_buf, out_ip)) return 0;
    if (*p != '/') return 1;
    p++;
    if (!parse_uint(&p, 32u, &prefix) || *p != '\0') return 0;
    return prefix_to_mask(prefix, out_mask);
}

static int get_info(sys_netinfo_t* info) {
    memset(info, 0, sizeof(*info));
    if (sys_netinfo(info) < 0) {
        u_puts("ip: netinfo unavailable\n");
        return 0;
    }
    return 1;
}

static void show_link(const sys_netinfo_t* info) {
    u_puts("link: ");
    u_puts(info->net_link_up ? "up" : "down");
    u_puts("  driver ");
    u_puts(info->net_driver);
    u_puts("  mac ");
    put_mac(info->mac);
    u_putc('\n');
}

static void show_addr(const sys_netinfo_t* info) {
    int exact = 0;
    uint32_t prefix;

    if (!info->ipv4_configured) {
        u_puts("inet: unconfigured\n");
        return;
    }

    prefix = mask_to_prefix(info->netmask, &exact);
    u_puts("inet: ");
    put_ip(info->ip);
    if (exact) {
        u_putc('/');
        u_put_uint(prefix);
    }
    u_puts("  netmask ");
    put_ip(info->netmask);
    u_putc('\n');
}

static void show_route(const sys_netinfo_t* info) {
    if (!info->ipv4_configured) {
        u_puts("route: unconfigured\n");
        return;
    }
    u_puts("route: connected ");
    put_ip(info->ip & info->netmask);
    u_puts(" netmask ");
    put_ip(info->netmask);
    u_putc('\n');
    if (info->gateway) {
        u_puts("route: default via ");
        put_ip(info->gateway);
        u_putc('\n');
    } else {
        u_puts("route: no default gateway\n");
    }
}

static void show_dns(const sys_netinfo_t* info) {
    if (info->dns) {
        u_puts("dns: ");
        put_ip(info->dns);
        u_putc('\n');
    } else {
        u_puts("dns: none\n");
    }
}

static void show_sockets(const sys_netinfo_t* info) {
    u_puts("sockets: ");
    u_put_uint(info->used_sockets);
    u_putc('/');
    u_put_uint(info->max_sockets);
    u_puts(" used tcp=");
    u_put_uint(info->tcp_sockets);
    u_puts(" listen=");
    u_put_uint(info->listening_sockets);
    u_puts(" connected=");
    u_put_uint(info->connected_sockets);
    u_putc('\n');
}

static void show_config(show_mode_t mode) {
    sys_netinfo_t info;

    if (!get_info(&info)) return;

    if (mode == SHOW_ADDR) {
        show_addr(&info);
        return;
    }
    if (mode == SHOW_ROUTE) {
        show_route(&info);
        return;
    }
    if (mode == SHOW_DNS) {
        show_dns(&info);
        return;
    }

    show_link(&info);
    show_addr(&info);
    if (mode == SHOW_SUMMARY) {
        if (info.ipv4_configured) {
            u_puts("gateway: ");
            if (info.gateway) put_ip(info.gateway);
            else u_puts("none");
            u_puts("  dns: ");
            if (info.dns) put_ip(info.dns);
            else u_puts("none");
            u_putc('\n');
        }
        return;
    }

    show_route(&info);
    show_dns(&info);
    if (info.dhcp_server) {
        u_puts("dhcp-server: ");
        put_ip(info.dhcp_server);
        u_putc('\n');
    }
    if (info.lease_seconds) {
        u_puts("lease: ");
        u_put_uint(info.lease_seconds);
        u_puts(" seconds\n");
    }
    show_sockets(&info);
}

static void usage(void) {
    u_puts("usage:\n");
    u_puts("  ip [show|-a]\n");
    u_puts("  ip addr show\n");
    u_puts("  ip addr add <ip>/<prefix> [gateway <gw>] [dns <dns>]\n");
    u_puts("  ip addr del|flush|clear\n");
    u_puts("  ip route show\n");
    u_puts("  ip route add default via <gw>\n");
    u_puts("  ip route del default\n");
    u_puts("  ip dns show|set <dns>|clear\n");
    u_puts("  ip dhcp|renew|release\n");
    u_puts("  ip set <ip> netmask <mask> [gateway <gw>] [dns <dns>]\n");
    u_puts("  ip ping <ip>\n");
    u_puts("  ip arp <ip>\n");
    u_puts("  ipconfig [/all|/renew|/release]\n");
}

static int configure_ipv4(uint32_t ip,
                          uint32_t mask,
                          uint32_t gateway,
                          uint32_t dns,
                          uint32_t dhcp_server,
                          uint32_t lease_seconds) {
    sys_net_op_request_t req;

    memset(&req, 0, sizeof(req));
    req.op = SYS_NET_OP_CONFIGURE;
    req.target_ip = ip;
    req.netmask = mask;
    req.gateway = gateway;
    req.dns = dns;
    req.dhcp_server = dhcp_server;
    req.lease_seconds = lease_seconds;
    return sys_net_op(&req) > 0;
}

static int configure_from_info(const sys_netinfo_t* info,
                               uint32_t ip,
                               uint32_t mask,
                               uint32_t gateway,
                               uint32_t dns) {
    uint32_t lease = info->lease_seconds;
    uint32_t dhcp_server = info->dhcp_server;

    if (ip != info->ip || mask != info->netmask) {
        lease = 0u;
        dhcp_server = 0u;
    }
    return configure_ipv4(ip, mask, gateway, dns, dhcp_server, lease);
}

static int run_dhcp(void) {
    sys_net_op_request_t req;
    memset(&req, 0, sizeof(req));
    req.op = SYS_NET_OP_DHCP;
    if (sys_net_op(&req) <= 0) {
        u_puts("ip: dhcp failed\n");
        return 1;
    }
    show_config(SHOW_ALL);
    return 0;
}

static int run_clear(void) {
    sys_net_op_request_t req;
    memset(&req, 0, sizeof(req));
    req.op = SYS_NET_OP_CLEAR_CONFIG;
    if (sys_net_op(&req) <= 0) {
        u_puts("ip: clear failed\n");
        return 1;
    }
    u_puts("ip: IPv4 config cleared\n");
    return 0;
}

static int run_addr_add(int argc, char** argv) {
    uint32_t ip;
    uint32_t mask = 0xFFFFFF00u;
    uint32_t gateway = 0;
    uint32_t dns = 0;

    if (argc < 4 || !parse_ip_prefix(argv[3], &ip, &mask)) {
        u_puts("ip: invalid address\n");
        return 1;
    }

    for (int i = 4; i < argc; i++) {
        if ((streq(argv[i], "gateway") || streq(argv[i], "gw") || streq(argv[i], "via")) && i + 1 < argc) {
            if (!parse_ip(argv[++i], &gateway)) {
                u_puts("ip: invalid gateway\n");
                return 1;
            }
        } else if (streq(argv[i], "dns") && i + 1 < argc) {
            if (!parse_ip(argv[++i], &dns)) {
                u_puts("ip: invalid dns\n");
                return 1;
            }
        } else {
            usage();
            return 1;
        }
    }

    if (!configure_ipv4(ip, mask, gateway, dns, 0u, 0u)) {
        u_puts("ip: address add failed\n");
        return 1;
    }
    show_config(SHOW_ALL);
    return 0;
}

static int run_set(int argc, char** argv) {
    sys_netinfo_t info;
    uint32_t ip;
    uint32_t mask = 0xFFFFFF00u;
    uint32_t gateway = 0;
    uint32_t dns = 0;
    int have_netmask = 0;

    if (!get_info(&info)) return 1;
    if (info.ipv4_configured) {
        gateway = info.gateway;
        dns = info.dns;
    }

    if (argc < 3 || !parse_ip_prefix(argv[2], &ip, &mask)) {
        u_puts("ip: invalid address\n");
        return 1;
    }

    for (int i = 3; i < argc; i++) {
        if ((streq(argv[i], "netmask") || streq(argv[i], "mask")) && i + 1 < argc) {
            if (!parse_ip(argv[++i], &mask)) {
                u_puts("ip: invalid netmask\n");
                return 1;
            }
            have_netmask = 1;
        } else if ((streq(argv[i], "gateway") || streq(argv[i], "gw") || streq(argv[i], "via")) && i + 1 < argc) {
            if (!parse_ip(argv[++i], &gateway)) {
                u_puts("ip: invalid gateway\n");
                return 1;
            }
        } else if (streq(argv[i], "dns") && i + 1 < argc) {
            if (!parse_ip(argv[++i], &dns)) {
                u_puts("ip: invalid dns\n");
                return 1;
            }
        } else {
            usage();
            return 1;
        }
    }

    if (!have_netmask && !contains_char(argv[2], '/')) {
        u_puts("ip: using default netmask 255.255.255.0\n");
    }

    if (!configure_ipv4(ip, mask, gateway, dns, 0u, 0u)) {
        u_puts("ip: set failed\n");
        return 1;
    }
    show_config(SHOW_ALL);
    return 0;
}

static int run_route(int argc, char** argv) {
    sys_netinfo_t info;
    uint32_t gateway;

    if (argc == 2 || (argc == 3 && streq(argv[2], "show"))) {
        show_config(SHOW_ROUTE);
        return 0;
    }
    if (!get_info(&info)) return 1;
    if (!info.ipv4_configured) {
        u_puts("ip: address is unconfigured\n");
        return 1;
    }

    if (argc == 6 && streq(argv[2], "add") && streq(argv[3], "default") && streq(argv[4], "via")) {
        if (!parse_ip(argv[5], &gateway)) {
            u_puts("ip: invalid gateway\n");
            return 1;
        }
        if (!configure_from_info(&info, info.ip, info.netmask, gateway, info.dns)) {
            u_puts("ip: route add failed\n");
            return 1;
        }
        show_config(SHOW_ROUTE);
        return 0;
    }

    if (argc == 4 && (streq(argv[2], "del") || streq(argv[2], "delete")) && streq(argv[3], "default")) {
        if (!configure_from_info(&info, info.ip, info.netmask, 0u, info.dns)) {
            u_puts("ip: route delete failed\n");
            return 1;
        }
        show_config(SHOW_ROUTE);
        return 0;
    }

    usage();
    return 1;
}

static int run_dns(int argc, char** argv) {
    sys_netinfo_t info;
    uint32_t dns = 0;

    if (argc == 2 || (argc == 3 && streq(argv[2], "show"))) {
        show_config(SHOW_DNS);
        return 0;
    }
    if (!get_info(&info)) return 1;
    if (!info.ipv4_configured) {
        u_puts("ip: address is unconfigured\n");
        return 1;
    }

    if (argc == 4 && streq(argv[2], "set")) {
        if (!parse_ip(argv[3], &dns)) {
            u_puts("ip: invalid dns\n");
            return 1;
        }
    } else if (argc == 3 && (streq(argv[2], "clear") || streq(argv[2], "del"))) {
        dns = 0;
    } else {
        usage();
        return 1;
    }

    if (!configure_from_info(&info, info.ip, info.netmask, info.gateway, dns)) {
        u_puts("ip: dns update failed\n");
        return 1;
    }
    show_config(SHOW_DNS);
    return 0;
}

static int run_ping(int argc, char** argv) {
    sys_net_op_request_t req;
    int rc;

    memset(&req, 0, sizeof(req));
    req.op = SYS_NET_OP_PING;

    if (argc < 3 || !parse_ip(argv[2], &req.target_ip)) {
        u_puts("ip: invalid ping target\n");
        return 1;
    }

    rc = sys_net_op(&req);
    if (rc < 0) {
        u_puts("ip: network unreachable\n");
        return 1;
    }
    u_puts(rc > 0 ? "ip: ping ok\n" : "ip: ping failed\n");
    return rc > 0 ? 0 : 1;
}

static int run_arp(int argc, char** argv) {
    sys_net_op_request_t req;
    int rc;

    memset(&req, 0, sizeof(req));
    req.op = SYS_NET_OP_ARP;

    if (argc < 3 || !parse_ip(argv[2], &req.target_ip)) {
        u_puts("ip: invalid arp target\n");
        return 1;
    }

    rc = sys_net_op(&req);
    if (rc < 0) {
        u_puts("ip: network unreachable\n");
        return 1;
    }
    if (rc == 0) {
        u_puts("ip: arp no reply\n");
        return 1;
    }
    u_puts("arp: ");
    put_ip(req.next_hop_ip);
    u_puts(" is-at ");
    put_mac(req.mac);
    u_putc('\n');
    return 0;
}

static int run_addr(int argc, char** argv) {
    if (argc == 2 || (argc == 3 && streq(argv[2], "show"))) {
        show_config(SHOW_ADDR);
        return 0;
    }
    if (argc >= 4 && (streq(argv[2], "add") || streq(argv[2], "set"))) {
        return run_addr_add(argc, argv);
    }
    if (argc == 3 && (streq(argv[2], "del") || streq(argv[2], "delete") ||
                      streq(argv[2], "flush") || streq(argv[2], "clear"))) {
        return run_clear();
    }
    usage();
    return 1;
}

static int run_ipconfig(int argc, char** argv) {
    if (argc == 1 || (argc == 2 && streq(argv[1], "/all"))) {
        show_config(argc == 2 ? SHOW_ALL : SHOW_SUMMARY);
        return 0;
    }
    if (argc == 2 && (streq(argv[1], "/renew") || streq(argv[1], "renew"))) {
        return run_dhcp();
    }
    if (argc == 2 && (streq(argv[1], "/release") || streq(argv[1], "release"))) {
        return run_clear();
    }
    usage();
    return 1;
}

void _start(int argc, char** argv) {
    int rc = 0;

    if (argc > 0 && argv[0] && starts_with(basename(argv[0]), "ipconfig")) {
        rc = run_ipconfig(argc, argv);
    } else if (argc == 1 || (argc == 2 && (streq(argv[1], "show") || streq(argv[1], "-a") ||
                                          streq(argv[1], "-all") || streq(argv[1], "--all")))) {
        show_config(argc == 2 ? SHOW_ALL : SHOW_SUMMARY);
    } else if (argc >= 2 && streq(argv[1], "addr")) {
        rc = run_addr(argc, argv);
    } else if (argc >= 2 && streq(argv[1], "route")) {
        rc = run_route(argc, argv);
    } else if (argc >= 2 && streq(argv[1], "dns")) {
        rc = run_dns(argc, argv);
    } else if (argc == 2 && (streq(argv[1], "dhcp") || streq(argv[1], "renew"))) {
        rc = run_dhcp();
    } else if (argc == 2 && (streq(argv[1], "clear") || streq(argv[1], "release"))) {
        rc = run_clear();
    } else if (argc >= 3 && streq(argv[1], "set")) {
        rc = run_set(argc, argv);
    } else if (argc >= 3 && streq(argv[1], "ping")) {
        rc = run_ping(argc, argv);
    } else if (argc >= 3 && streq(argv[1], "arp")) {
        rc = run_arp(argc, argv);
    } else if (argc == 2 && (streq(argv[1], "help") || streq(argv[1], "--help") || streq(argv[1], "-h"))) {
        usage();
    } else {
        usage();
        rc = 1;
    }

    sys_exit(rc);
}
