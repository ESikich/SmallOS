#include "syscall_internal.h"
#include "klib.h"
#include "process.h"
#include "scheduler.h"
#include "socket.h"
#include "uapi_errno.h"
#include "uapi_net.h"
#include "uapi_netlink.h"
#include "wait.h"
#include "../drivers/arp.h"
#include "../drivers/dhcp.h"
#include "../drivers/ipv4.h"
#include "../drivers/net.h"
#include "../drivers/nic.h"
#include "../drivers/tcp.h"

#define SYS_NETLINK_MAX_PACKET 4096u

static unsigned int net_route_next_hop(unsigned int target_ip);
static int sys_udp_send_socket(fd_entry_t* ent,
                               const void* buf,
                               unsigned int len,
                               const struct sockaddr* dest_addr,
                               unsigned int addrlen);

static int copy_user_sockaddr_in(struct sockaddr_in* dst,
                                 const struct sockaddr* src,
                                 unsigned int len) {
    if (!dst || !src) {
        return -EFAULT;
    }
    if (len < sizeof(struct sockaddr_in)) {
        return -EINVAL;
    }
    if (!user_buf_ok((unsigned int)src, sizeof(struct sockaddr_in))) {
        return -EFAULT;
    }

    if (copy_from_user(dst, src, sizeof(struct sockaddr_in)) < 0) {
        return -EFAULT;
    }
    if (dst->sin_family != AF_INET) {
        return -EINVAL;
    }
    return 0;
}

static int socket_fd_is_socket(fd_entry_t* ent) {
    return ent && ent->valid && ent->kind == PROCESS_HANDLE_KIND_SOCKET && ent->socket;
}

static unsigned short swap_u16(unsigned short value) {
    return (unsigned short)(((value & 0x00FFu) << 8) | ((value & 0xFF00u) >> 8));
}

static unsigned int swap_u32(unsigned int value) {
    return ((value & 0x000000FFu) << 24)
         | ((value & 0x0000FF00u) << 8)
         | ((value & 0x00FF0000u) >> 8)
         | ((value & 0xFF000000u) >> 24);
}

int sys_socket_impl(int domain, int type, int protocol) {
    process_t* proc = (process_t*)sched_current();
    int fd;
    int sock_type = type & SOCK_TYPE_MASK;
    unsigned int fd_flags = 0u;
    socket_kind_t kind = SOCKET_KIND_NONE;

    if (!proc) return -EINVAL;
    if ((type & ~(SOCK_TYPE_MASK | SOCK_NONBLOCK | SOCK_CLOEXEC)) != 0) return -EINVAL;
    if (domain == AF_NETLINK) {
        if (protocol != NETLINK_ROUTE) return -EPROTONOSUPPORT;
        if (sock_type != SOCK_RAW && sock_type != SOCK_DGRAM) return -EPROTONOSUPPORT;
        kind = SOCKET_KIND_NETLINK_ROUTE;
    } else if (domain != AF_INET) {
        return -EAFNOSUPPORT;
    } else if (sock_type == SOCK_STREAM) {
        if (protocol != 0 && protocol != IPPROTO_TCP) return -EPROTONOSUPPORT;
        kind = SOCKET_KIND_TCP;
    } else if (sock_type == SOCK_DGRAM) {
        if (protocol != 0 && protocol != IPPROTO_UDP) return -EPROTONOSUPPORT;
        kind = SOCKET_KIND_UDP;
    } else if (sock_type == SOCK_RAW) {
        if (protocol != IPPROTO_ICMP) return -EPROTONOSUPPORT;
        kind = SOCKET_KIND_RAW_ICMP;
    } else {
        return -EPROTONOSUPPORT;
    }

    fd = process_fd_open_socket_kind(proc, "socket", kind);
    if (fd < 0) return fd;
    if ((type & SOCK_NONBLOCK) != 0) {
        fd_entry_t* ent = process_fd_get(proc, fd);
        fd_flags |= SYS_FD_FLAG_NONBLOCK;
        (void)process_fd_set_flags(ent, fd_flags);
    }
    if ((type & SOCK_CLOEXEC) != 0) {
        fd_entry_t* ent = process_fd_get(proc, fd);
        (void)process_fd_set_fd_flags(ent, SYS_FD_FLAG_CLOEXEC);
    }
    return fd;
}

int sys_bind_impl(int fd, const struct sockaddr* addr, unsigned int addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_state(ent->socket) != SOCKET_STATE_OPEN) return -EINVAL;
    if (socket_kind(ent->socket) == SOCKET_KIND_NETLINK_ROUTE) {
        struct sockaddr_nl nl;
        if (!addr || addrlen < sizeof(nl)) return -EINVAL;
        if (!user_buf_ok((unsigned int)addr, sizeof(nl))) return -EFAULT;
        if (copy_from_user(&nl, addr, sizeof(nl)) < 0) return -EFAULT;
        if (nl.nl_family != AF_NETLINK) return -EINVAL;
        if (nl.nl_pid == 0u) nl.nl_pid = (unsigned int)proc->pid;
        return socket_bind_netlink(ent->socket, nl.nl_pid, nl.nl_groups);
    }
    int sa_rc = copy_user_sockaddr_in(&sa, addr, addrlen);
    if (sa_rc < 0) return sa_rc;

    if (socket_kind(ent->socket) == SOCKET_KIND_UDP) {
        sa_rc = socket_bind_udp(ent->socket, swap_u16(sa.sin_port));
    } else {
        sa_rc = socket_bind_tcp(ent->socket, swap_u16(sa.sin_port));
    }
    if (sa_rc < 0) return sa_rc;
    ent->socket_port = socket_local_port(ent->socket);
    ent->socket_state = PROCESS_SOCKET_STATE_BOUND;
    return 0;
}

int sys_listen_impl(int fd, int backlog) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    (void)backlog;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_state(ent->socket) != SOCKET_STATE_BOUND) return -EINVAL;
    if (socket_local_port(ent->socket) == 0u) return -EINVAL;

    int listen_rc = socket_listen_tcp(ent->socket, backlog);
    if (listen_rc < 0) return listen_rc;
    ent->socket_port = socket_local_port(ent->socket);
    ent->socket_state = PROCESS_SOCKET_STATE_LISTENER;
    return 0;
}

int sys_accept_impl(syscall_regs_t* regs,
                           int fd,
                           struct sockaddr* addr,
                           unsigned int* addrlen,
                           unsigned int flags) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    fd_entry_t* new_ent;
    unsigned int peer_ip;
    unsigned int peer_port;
    int new_fd;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_state(ent->socket) != SOCKET_STATE_LISTENING) return -EINVAL;
    if ((flags & ~(SOCK_NONBLOCK | SOCK_CLOEXEC)) != 0u) return -EINVAL;

    if (!socket_accept_ready(ent->socket) &&
        ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u ||
         (flags & SOCK_NONBLOCK) != 0u)) {
        return -EAGAIN;
    }

    while (!socket_accept_ready(ent->socket)) {
        int wait_rc;

        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_accept_ready(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);

    new_fd = process_fd_open_socket(proc, "socket");
    if (new_fd < 0) return new_fd;
    new_ent = process_fd_get(proc, new_fd);
    if (!socket_fd_is_socket(new_ent)) {
        process_fd_close(new_ent);
        return -EBADF;
    }
    if (socket_accept_tcp(ent->socket, new_ent->socket) < 0) {
        process_fd_close(new_ent);
        return -EAGAIN;
    }
    new_ent->socket_state = PROCESS_SOCKET_STATE_CONNECTED;
    new_ent->socket_port = socket_local_port(new_ent->socket);
    new_ent->socket_conn = socket_conn_id(new_ent->socket);
    if ((flags & SOCK_NONBLOCK) != 0u) {
        new_ent->flags |= SYS_FD_FLAG_NONBLOCK;
    }
    if ((flags & SOCK_CLOEXEC) != 0u) {
        new_ent->fd_flags |= SYS_FD_FLAG_CLOEXEC;
    }

    peer_ip = socket_peer_ip(new_ent->socket);
    peer_port = socket_peer_port(new_ent->socket);
    if (addr && addrlen) {
        struct sockaddr_in sa;
        unsigned int user_addrlen = 0;
        if (read_user_u32(&user_addrlen, addrlen) < 0) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -EFAULT;
        }
        if (user_addrlen < sizeof(struct sockaddr_in)) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -EINVAL;
        }
        if (!user_buf_ok((unsigned int)addr, sizeof(struct sockaddr_in))) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -EFAULT;
        }
        k_memset(&sa, 0, sizeof(sa));
        sa.sin_family = AF_INET;
        sa.sin_port = swap_u16((unsigned short)peer_port);
        sa.sin_addr.s_addr = swap_u32(peer_ip);
        if (copy_to_user(addr, &sa, sizeof(sa)) < 0 ||
            write_user_u32(addrlen, sizeof(sa)) < 0) {
            process_fd_close(process_fd_get(proc, new_fd));
            return -EFAULT;
        }
    }

    return new_fd;
}

int sys_connect_impl(int fd, const struct sockaddr* addr, unsigned int addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;
    unsigned int remote_ip;
    unsigned int remote_port;
    int rc;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    rc = copy_user_sockaddr_in(&sa, addr, addrlen);
    if (rc < 0) return rc;

    remote_ip = swap_u32(sa.sin_addr.s_addr);
    remote_port = swap_u16(sa.sin_port);
    if (socket_kind(ent->socket) == SOCKET_KIND_UDP) {
        rc = socket_connect_udp(ent->socket, remote_ip, remote_port);
        if (rc < 0) return rc;
        ent->socket_state = PROCESS_SOCKET_STATE_CONNECTED;
        ent->socket_port = socket_local_port(ent->socket);
        return 0;
    }

    rc = socket_connect_tcp(ent->socket, remote_ip, remote_port);
    if (rc == -EALREADY && (ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
        return -EALREADY;
    }
    if (rc < 0) return rc;

    ent->socket_state = socket_tcp_connection_established(ent->socket)
                      ? PROCESS_SOCKET_STATE_CONNECTED
                      : PROCESS_SOCKET_STATE_CONNECTING;
    ent->socket_port = socket_local_port(ent->socket);
    ent->socket_conn = socket_conn_id(ent->socket);

    if ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
        if (!socket_tcp_connection_established(ent->socket)) {
            return -EINPROGRESS;
        }
        ent->socket_state = PROCESS_SOCKET_STATE_CONNECTED;
        return 0;
    }

    while (!socket_tcp_connection_established(ent->socket)) {
        int wait_rc;

        if (!socket_tcp_connect_pending(ent->socket)) {
            socket_wait_clear_process(proc);
            return -ECONNREFUSED;
        }
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLOUT);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_tcp_connection_established(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);
    ent->socket_state = PROCESS_SOCKET_STATE_CONNECTED;
    return 0;
}

typedef struct {
    unsigned char* buf;
    unsigned int cap;
    unsigned int len;
    unsigned int seq;
    unsigned int pid;
    unsigned int multi;
} netlink_builder_t;

static unsigned int net_mask_from_prefix(unsigned int prefix) {
    if (prefix == 0u) return 0u;
    if (prefix >= 32u) return 0xFFFFFFFFu;
    return 0xFFFFFFFFu << (32u - prefix);
}

static unsigned int net_prefix_from_mask(unsigned int mask) {
    unsigned int prefix = 0u;
    for (int bit = 31; bit >= 0; bit--) {
        if ((mask & (1u << (unsigned int)bit)) == 0u) break;
        prefix++;
    }
    return prefix;
}

static unsigned int net_eth0_flags(void) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    unsigned int flags = IFF_BROADCAST | IFF_MULTICAST;
    if (cfg && cfg->configured) flags |= IFF_UP;
    if (nic_link_up()) flags |= IFF_RUNNING;
    return flags;
}

static unsigned int net_loopback_flags(void) {
    return IFF_UP | IFF_LOOPBACK | IFF_RUNNING;
}

static void* nl_begin(netlink_builder_t* b,
                      unsigned int type,
                      unsigned int payload_len,
                      unsigned int flags) {
    unsigned int off;
    unsigned int total;
    struct nlmsghdr* nlh;

    if (!b || !b->buf) return 0;
    off = NLMSG_ALIGN(b->len);
    total = NLMSG_LENGTH(payload_len);
    if (off + total > b->cap) return 0;
    if (off > b->len) {
        k_memset(b->buf + b->len, 0, off - b->len);
    }
    nlh = (struct nlmsghdr*)(b->buf + off);
    k_memset(nlh, 0, total);
    nlh->nlmsg_len = total;
    nlh->nlmsg_type = (unsigned short)type;
    nlh->nlmsg_flags = (unsigned short)(flags | (b->multi ? NLM_F_MULTI : 0u));
    nlh->nlmsg_seq = b->seq;
    nlh->nlmsg_pid = b->pid;
    b->len = off + total;
    return NLMSG_DATA(nlh);
}

static int nl_attr_put(netlink_builder_t* b,
                       struct nlmsghdr* nlh,
                       unsigned int type,
                       const void* data,
                       unsigned int len) {
    unsigned int msg_off;
    unsigned int attr_off;
    unsigned int attr_len = RTA_LENGTH(len);
    struct rtattr* rta;

    if (!b || !nlh || !data) return -EINVAL;
    msg_off = (unsigned int)((unsigned char*)nlh - b->buf);
    attr_off = msg_off + NLMSG_ALIGN(nlh->nlmsg_len);
    if (attr_off + attr_len > b->cap) return -EMSGSIZE;
    if (attr_off > b->len) {
        k_memset(b->buf + b->len, 0, attr_off - b->len);
    }
    rta = (struct rtattr*)(b->buf + attr_off);
    k_memset(rta, 0, RTA_ALIGN(attr_len));
    rta->rta_type = (unsigned short)type;
    rta->rta_len = (unsigned short)attr_len;
    k_memcpy(RTA_DATA(rta), data, len);
    nlh->nlmsg_len = NLMSG_ALIGN(nlh->nlmsg_len) + attr_len;
    b->len = msg_off + nlh->nlmsg_len;
    return 0;
}

static int nl_attr_put_u32(netlink_builder_t* b,
                           struct nlmsghdr* nlh,
                           unsigned int type,
                           unsigned int value) {
    return nl_attr_put(b, nlh, type, &value, sizeof(value));
}

static int nl_attr_put_str(netlink_builder_t* b,
                           struct nlmsghdr* nlh,
                           unsigned int type,
                           const char* value) {
    return nl_attr_put(b, nlh, type, value, (unsigned int)k_strlen(value) + 1u);
}

static int nl_put_done(netlink_builder_t* b) {
    return nl_begin(b, NLMSG_DONE, 0u, 0u) ? 0 : -EMSGSIZE;
}

static int nl_put_ack(netlink_builder_t* b,
                      const struct nlmsghdr* req,
                      int error) {
    struct nlmsgerr* err = (struct nlmsgerr*)nl_begin(b, NLMSG_ERROR,
                                                      sizeof(*err), 0u);
    if (!err) return -EMSGSIZE;
    err->error = error;
    if (req) {
        err->msg = *req;
    } else {
        k_memset(&err->msg, 0, sizeof(err->msg));
    }
    return 0;
}

static int nl_put_link(netlink_builder_t* b,
                       const char* name,
                       int ifindex,
                       unsigned short type,
                       unsigned int flags,
                       const unsigned char* hwaddr,
                       unsigned int hwaddr_len,
                       unsigned int mtu,
                       const unsigned char* bcast,
                       unsigned int bcast_len) {
    struct ifinfomsg* ifi;
    struct nlmsghdr* nlh;
    unsigned char operstate = 6u;

    ifi = (struct ifinfomsg*)nl_begin(b, RTM_NEWLINK, sizeof(*ifi), 0u);
    if (!ifi) return -EMSGSIZE;
    nlh = (struct nlmsghdr*)((unsigned char*)ifi - NLMSG_LENGTH(0));
    ifi->ifi_family = AF_UNSPEC;
    ifi->ifi_type = type;
    ifi->ifi_index = ifindex;
    ifi->ifi_flags = flags;
    ifi->ifi_change = 0xFFFFFFFFu;

    if (nl_attr_put_str(b, nlh, IFLA_IFNAME, name) < 0) return -EMSGSIZE;
    if (hwaddr && hwaddr_len != 0u &&
        nl_attr_put(b, nlh, IFLA_ADDRESS, hwaddr, hwaddr_len) < 0) {
        return -EMSGSIZE;
    }
    if (bcast && bcast_len != 0u &&
        nl_attr_put(b, nlh, IFLA_BROADCAST, bcast, bcast_len) < 0) {
        return -EMSGSIZE;
    }
    if (nl_attr_put_u32(b, nlh, IFLA_MTU, mtu) < 0) return -EMSGSIZE;
    if (nl_attr_put(b, nlh, IFLA_OPERSTATE, &operstate, sizeof(operstate)) < 0) {
        return -EMSGSIZE;
    }
    return 0;
}

static int nl_put_eth0_link(netlink_builder_t* b) {
    const u8* mac = nic_mac();
    unsigned char bcast[ETH_ALEN];

    k_memset(bcast, 0xFF, sizeof(bcast));
    return nl_put_link(b, "eth0", 1, ARPHRD_ETHER, net_eth0_flags(),
                       mac, mac ? ETH_ALEN : 0u, 1500u, bcast, sizeof(bcast));
}

static int nl_put_loopback_link(netlink_builder_t* b) {
    unsigned char zero[ETH_ALEN];

    k_memset(zero, 0, sizeof(zero));
    return nl_put_link(b, "lo", 2, ARPHRD_LOOPBACK, net_loopback_flags(),
                       zero, sizeof(zero), 65536u, zero, sizeof(zero));
}

static int nl_put_addr_entry(netlink_builder_t* b,
                             const char* label,
                             unsigned int ifindex,
                             unsigned int ip,
                             unsigned int prefix,
                             unsigned int scope,
                             unsigned int flags) {
    struct ifaddrmsg* ifa;
    struct nlmsghdr* nlh;
    unsigned int ip_be;
    unsigned int mask = net_mask_from_prefix(prefix);
    unsigned int bcast_be;

    if (ip == 0u) return 0;
    ifa = (struct ifaddrmsg*)nl_begin(b, RTM_NEWADDR, sizeof(*ifa), 0u);
    if (!ifa) return -EMSGSIZE;
    nlh = (struct nlmsghdr*)((unsigned char*)ifa - NLMSG_LENGTH(0));
    ifa->ifa_family = AF_INET;
    ifa->ifa_prefixlen = (unsigned char)prefix;
    ifa->ifa_flags = (unsigned char)flags;
    ifa->ifa_scope = (unsigned char)scope;
    ifa->ifa_index = ifindex;

    ip_be = swap_u32(ip);
    bcast_be = swap_u32((ip & mask) | (~mask));
    if (nl_attr_put_u32(b, nlh, IFA_ADDRESS, ip_be) < 0) return -EMSGSIZE;
    if (nl_attr_put_u32(b, nlh, IFA_LOCAL, ip_be) < 0) return -EMSGSIZE;
    if (scope != RT_SCOPE_HOST &&
        nl_attr_put_u32(b, nlh, IFA_BROADCAST, bcast_be) < 0) {
        return -EMSGSIZE;
    }
    if (nl_attr_put_str(b, nlh, IFA_LABEL, label) < 0) return -EMSGSIZE;
    return 0;
}

static int nl_put_addr(netlink_builder_t* b) {
    const net_ipv4_config_t* cfg = net_ipv4_config();

    if (nl_put_addr_entry(b, "lo", 2u, 0x7F000001u, 8u,
                          RT_SCOPE_HOST, IFA_F_PERMANENT) < 0) {
        return -EMSGSIZE;
    }
    if (!cfg || !cfg->configured || cfg->ip == 0u) return 0;
    if (nl_put_addr_entry(b, "eth0", 1u, cfg->ip,
                          net_prefix_from_mask(cfg->netmask),
                          RT_SCOPE_UNIVERSE, IFA_F_PERMANENT) < 0) {
        return -EMSGSIZE;
    }
    return 0;
}

static int nl_put_route(netlink_builder_t* b,
                        unsigned int dst,
                        unsigned int prefix,
                        unsigned int gateway,
                        unsigned int scope,
                        unsigned int proto) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    struct rtmsg* rtm;
    struct nlmsghdr* nlh;
    unsigned int ifindex = 1u;

    if (!cfg || !cfg->configured) return 0;
    rtm = (struct rtmsg*)nl_begin(b, RTM_NEWROUTE, sizeof(*rtm), 0u);
    if (!rtm) return -EMSGSIZE;
    nlh = (struct nlmsghdr*)((unsigned char*)rtm - NLMSG_LENGTH(0));
    rtm->rtm_family = AF_INET;
    rtm->rtm_dst_len = (unsigned char)prefix;
    rtm->rtm_table = RT_TABLE_MAIN;
    rtm->rtm_protocol = (unsigned char)proto;
    rtm->rtm_scope = (unsigned char)scope;
    rtm->rtm_type = RTN_UNICAST;

    if (prefix != 0u) {
        unsigned int dst_be = swap_u32(dst);
        if (nl_attr_put_u32(b, nlh, RTA_DST, dst_be) < 0) return -EMSGSIZE;
    }
    if (gateway != 0u) {
        unsigned int gw_be = swap_u32(gateway);
        if (nl_attr_put_u32(b, nlh, RTA_GATEWAY, gw_be) < 0) return -EMSGSIZE;
    }
    if (nl_attr_put_u32(b, nlh, RTA_OIF, ifindex) < 0) return -EMSGSIZE;
    if (cfg->ip != 0u) {
        unsigned int src_be = swap_u32(cfg->ip);
        if (nl_attr_put_u32(b, nlh, RTA_PREFSRC, src_be) < 0) return -EMSGSIZE;
    }
    return 0;
}

static int nl_put_neigh(netlink_builder_t* b) {
    struct ndmsg* ndm;
    struct nlmsghdr* nlh;
    unsigned int sender_ip = 0u;
    unsigned int target_ip = 0u;
    unsigned int target_be;
    unsigned char mac[ETH_ALEN];

    if (!arp_cache_get(&sender_ip, &target_ip, mac)) return 0;
    if (sender_ip == 0u || target_ip == 0u) return 0;

    ndm = (struct ndmsg*)nl_begin(b, RTM_NEWNEIGH, sizeof(*ndm), 0u);
    if (!ndm) return -EMSGSIZE;
    nlh = (struct nlmsghdr*)((unsigned char*)ndm - NLMSG_LENGTH(0));
    ndm->ndm_family = AF_INET;
    ndm->ndm_ifindex = 1;
    ndm->ndm_state = NUD_REACHABLE;
    ndm->ndm_type = RTN_UNICAST;

    target_be = swap_u32(target_ip);
    if (nl_attr_put_u32(b, nlh, NDA_DST, target_be) < 0) return -EMSGSIZE;
    if (nl_attr_put(b, nlh, NDA_LLADDR, mac, sizeof(mac)) < 0) return -EMSGSIZE;
    return 0;
}

static void nl_parse_attrs(struct rtattr** attrs,
                           unsigned int max,
                           struct rtattr* rta,
                           int len) {
    for (unsigned int i = 0; i <= max; i++) attrs[i] = 0;
    while (RTA_OK(rta, len)) {
        if (rta->rta_type <= max) attrs[rta->rta_type] = rta;
        rta = RTA_NEXT(rta, len);
    }
}

static unsigned int nl_attr_u32(struct rtattr* rta) {
    unsigned int out = 0u;
    if (rta && RTA_PAYLOAD(rta) >= (int)sizeof(out)) {
        k_memcpy(&out, RTA_DATA(rta), sizeof(out));
    }
    return out;
}

static int nl_apply_addr(const struct nlmsghdr* nlh, int del) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    const struct ifaddrmsg* ifa = (const struct ifaddrmsg*)NLMSG_DATA(nlh);
    struct rtattr* attrs[IFA_MAX + 1u];
    unsigned int ip = cfg ? cfg->ip : 0u;
    unsigned int netmask = cfg && cfg->netmask ? cfg->netmask : 0xFFFFFF00u;
    unsigned int gateway = cfg ? cfg->gateway : 0u;
    unsigned int dns = cfg ? cfg->dns : 0u;
    unsigned int dhcp_server = cfg ? cfg->dhcp_server : 0u;
    unsigned int lease_seconds = cfg ? cfg->lease_seconds : 0u;
    unsigned int ip_be;

    if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*ifa))) return -EINVAL;
    if (ifa->ifa_family != AF_INET || ifa->ifa_index != 1u) return -ENODEV;
    nl_parse_attrs(attrs, IFA_MAX, IFA_RTA(ifa), IFA_PAYLOAD(nlh));
    ip_be = nl_attr_u32(attrs[IFA_LOCAL] ? attrs[IFA_LOCAL] : attrs[IFA_ADDRESS]);
    if (del) {
        if (ip_be != 0u && cfg && swap_u32(ip_be) != cfg->ip) return -EADDRNOTAVAIL;
        net_ipv4_configure(0u, netmask, gateway, dns, dhcp_server, lease_seconds);
        return 0;
    }
    ip = swap_u32(ip_be);
    if (ip == 0u) return -EINVAL;
    netmask = net_mask_from_prefix(ifa->ifa_prefixlen);
    net_ipv4_configure(ip, netmask, gateway, dns, dhcp_server, lease_seconds);
    return 0;
}

static int nl_apply_route(const struct nlmsghdr* nlh, int del) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    const struct rtmsg* rtm = (const struct rtmsg*)NLMSG_DATA(nlh);
    struct rtattr* attrs[RTA_MAX + 1u];
    unsigned int dst = 0u;
    unsigned int gateway = 0u;

    if (!cfg || !cfg->configured) return -ENETUNREACH;
    if (nlh->nlmsg_len < NLMSG_LENGTH(sizeof(*rtm))) return -EINVAL;
    if (rtm->rtm_family != AF_INET) return -EAFNOSUPPORT;
    nl_parse_attrs(attrs, RTA_MAX, RTM_RTA(rtm), RTM_PAYLOAD(nlh));
    if (attrs[RTA_DST]) dst = swap_u32(nl_attr_u32(attrs[RTA_DST]));
    if (attrs[RTA_GATEWAY]) gateway = swap_u32(nl_attr_u32(attrs[RTA_GATEWAY]));
    if (attrs[RTA_OIF] && nl_attr_u32(attrs[RTA_OIF]) != 1u) return -ENODEV;

    if (rtm->rtm_dst_len != 0u || dst != 0u) {
        return del ? 0 : -EOPNOTSUPP;
    }
    net_ipv4_configure(cfg->ip,
                       cfg->netmask,
                       del ? 0u : gateway,
                       cfg->dns,
                       cfg->dhcp_server,
                       cfg->lease_seconds);
    return 0;
}

static int nl_build_response(socket_t* sock,
                             const unsigned char* req_buf,
                             unsigned int req_len,
                             unsigned char* resp,
                             unsigned int resp_cap) {
    netlink_builder_t b;
    struct nlmsghdr* nlh;
    int remaining;
    int rc = 0;

    if (!sock || !req_buf || !resp || req_len < sizeof(struct nlmsghdr)) return -EINVAL;
    nlh = (struct nlmsghdr*)req_buf;
    remaining = (int)req_len;
    if (!NLMSG_OK(nlh, remaining)) return -EINVAL;

    k_memset(&b, 0, sizeof(b));
    b.buf = resp;
    b.cap = resp_cap;
    b.seq = nlh->nlmsg_seq;
    b.pid = socket_netlink_pid(sock);

    switch (nlh->nlmsg_type) {
    case RTM_GETLINK:
        b.multi = 1u;
        rc = nl_put_eth0_link(&b);
        if (rc == 0) rc = nl_put_loopback_link(&b);
        if (rc == 0) rc = nl_put_done(&b);
        break;
    case RTM_GETADDR:
        b.multi = 1u;
        rc = nl_put_addr(&b);
        if (rc == 0) rc = nl_put_done(&b);
        break;
    case RTM_GETROUTE:
    {
        const net_ipv4_config_t* cfg = net_ipv4_config();
        b.multi = 1u;
        if (cfg && cfg->configured) {
            unsigned int prefix = net_prefix_from_mask(cfg->netmask);
            unsigned int network = cfg->ip & cfg->netmask;
            if (prefix != 0u) {
                rc = nl_put_route(&b, network, prefix, 0u,
                                  RT_SCOPE_LINK, RTPROT_KERNEL);
            }
            if (rc == 0 && cfg->gateway != 0u) {
                rc = nl_put_route(&b, 0u, 0u, cfg->gateway,
                                  RT_SCOPE_UNIVERSE, RTPROT_DHCP);
            }
        }
        if (rc == 0) rc = nl_put_done(&b);
        break;
    }
    case RTM_GETNEIGH:
        b.multi = 1u;
        rc = nl_put_neigh(&b);
        if (rc == 0) rc = nl_put_done(&b);
        break;
    case RTM_NEWADDR:
        rc = nl_apply_addr(nlh, 0);
        (void)nl_put_ack(&b, nlh, rc < 0 ? rc : 0);
        break;
    case RTM_DELADDR:
        rc = nl_apply_addr(nlh, 1);
        (void)nl_put_ack(&b, nlh, rc < 0 ? rc : 0);
        break;
    case RTM_NEWROUTE:
        rc = nl_apply_route(nlh, 0);
        (void)nl_put_ack(&b, nlh, rc < 0 ? rc : 0);
        break;
    case RTM_DELROUTE:
        rc = nl_apply_route(nlh, 1);
        (void)nl_put_ack(&b, nlh, rc < 0 ? rc : 0);
        break;
    case RTM_NEWNEIGH:
    case RTM_DELNEIGH:
        (void)nl_put_ack(&b, nlh, 0);
        break;
    case RTM_SETLINK:
    case RTM_NEWLINK:
        (void)nl_put_ack(&b, nlh, 0);
        break;
    default:
        (void)nl_put_ack(&b, nlh, -EOPNOTSUPP);
        break;
    }

    if (b.len == 0u) return rc < 0 ? rc : -EINVAL;
    return socket_netlink_queue(sock, b.buf, b.len) < 0 ? -ENOMEM : (int)req_len;
}

int sys_netlink_send_user(fd_entry_t* ent,
                                 const void* buf,
                                 unsigned int len) {
    static unsigned char req[SYS_NETLINK_MAX_PACKET];
    static unsigned char resp[SYS_NETLINK_MAX_PACKET];

    if (!ent || !ent->socket || socket_kind(ent->socket) != SOCKET_KIND_NETLINK_ROUTE) {
        return -EINVAL;
    }
    if (!buf || len == 0u) return -EINVAL;
    if (len > sizeof(req)) return -EMSGSIZE;
    if (copy_from_user(req, buf, len) < 0) return -EFAULT;
    k_memset(resp, 0, sizeof(resp));
    return nl_build_response(ent->socket, req, len, resp, sizeof(resp));
}

static int sys_netlink_recv_user(process_t* proc,
                                 fd_entry_t* ent,
                                 void* buf,
                                 unsigned int len,
                                 unsigned int flags,
                                 struct sockaddr* src_addr,
                                 unsigned int* addrlen) {
    static unsigned char packet[SYS_NETLINK_MAX_PACKET];
    unsigned int src_pid = 0u;
    unsigned int src_groups = 0u;
    unsigned int peek = (flags & MSG_PEEK) != 0u;
    int rc;

    if (!proc || !ent || !buf) return -EINVAL;
    if (socket_kind(ent->socket) != SOCKET_KIND_NETLINK_ROUTE) return -EINVAL;
    if (!socket_netlink_recv_ready(ent->socket) &&
        ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u ||
         (flags & MSG_DONTWAIT) != 0u)) {
        return -EAGAIN;
    }
    while (!socket_netlink_recv_ready(ent->socket)) {
        int wait_rc;
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_netlink_recv_ready(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);

    rc = socket_netlink_recv(ent->socket, packet, sizeof(packet),
                             &src_pid, &src_groups, peek);
    if (rc < 0) return rc;
    if ((unsigned int)rc > len) {
        rc = (int)len;
    }
    if (copy_to_user(buf, packet, (unsigned int)rc) < 0) return -EFAULT;
    if (src_addr && addrlen) {
        struct sockaddr_nl sa;
        unsigned int user_addrlen = 0u;
        if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
        if (user_addrlen < sizeof(sa)) return -EINVAL;
        if (!user_buf_ok((unsigned int)src_addr, sizeof(sa))) return -EFAULT;
        k_memset(&sa, 0, sizeof(sa));
        sa.nl_family = AF_NETLINK;
        sa.nl_pid = src_pid;
        sa.nl_groups = src_groups;
        if (copy_to_user(src_addr, &sa, sizeof(sa)) < 0 ||
            write_user_u32(addrlen, sizeof(sa)) < 0) {
            return -EFAULT;
        }
    } else if (src_addr || addrlen) {
        return -EFAULT;
    }
    return rc;
}

static void net_write_u16_be(unsigned char* buf, unsigned int off, unsigned int value);

int sys_send_impl(int fd, const void* buf, unsigned int len) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    if (len == 0u) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_kind(ent->socket) == SOCKET_KIND_NETLINK_ROUTE) {
        return sys_netlink_send_user(ent, buf, len);
    }
    if (socket_kind(ent->socket) == SOCKET_KIND_UDP) {
        return sys_udp_send_socket(ent, buf, len, 0, 0u);
    }
    return process_fd_write(ent, (const char*)buf, len);
}

static int sys_raw_icmp_recv_socket(process_t* proc,
                                    fd_entry_t* ent,
                                    void* buf,
                                    unsigned int len,
                                    unsigned int flags,
                                    unsigned int* out_src_ip) {
    if (!proc || !ent || !buf) return -EINVAL;
    if (!socket_raw_icmp_recv_ready(ent->socket) &&
        ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u ||
         (flags & MSG_DONTWAIT) != 0u)) {
        return -EAGAIN;
    }

    while (!socket_raw_icmp_recv_ready(ent->socket)) {
        int wait_rc;
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_raw_icmp_recv_ready(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);

    return socket_raw_icmp_recv(ent->socket, buf, len, out_src_ip);
}

static int sys_udp_send_socket(fd_entry_t* ent,
                               const void* buf,
                               unsigned int len,
                               const struct sockaddr* dest_addr,
                               unsigned int addrlen) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    struct sockaddr_in sa;
    unsigned int target_ip;
    unsigned int target_port;
    unsigned int next_hop;
    unsigned int local_port;
    unsigned char packet[1488];
    int rc;

    if (!ent || !buf || socket_kind(ent->socket) != SOCKET_KIND_UDP) return -EINVAL;
    if (!cfg || !cfg->configured) return -ENETUNREACH;
    if (len > sizeof(packet) - 8u) return -EMSGSIZE;

    if (dest_addr) {
        rc = copy_user_sockaddr_in(&sa, dest_addr, addrlen);
        if (rc < 0) return rc;
        target_ip = swap_u32(sa.sin_addr.s_addr);
        target_port = swap_u16(sa.sin_port);
    } else {
        if (socket_state(ent->socket) != SOCKET_STATE_CONNECTED) return -EDESTADDRREQ;
        target_ip = socket_peer_ip(ent->socket);
        target_port = socket_peer_port(ent->socket);
    }
    if (target_ip == 0u || target_port == 0u || target_port > 0xFFFFu) {
        return -EDESTADDRREQ;
    }

    rc = socket_udp_ensure_bound(ent->socket);
    if (rc < 0) return rc;
    local_port = socket_local_port(ent->socket);
    if (local_port == 0u) return -EINVAL;

    if (copy_from_user(packet + 8u, buf, len) < 0) return -EFAULT;
    net_write_u16_be(packet, 0u, local_port);
    net_write_u16_be(packet, 2u, target_port);
    net_write_u16_be(packet, 4u, len + 8u);
    net_write_u16_be(packet, 6u, 0u);

    next_hop = net_route_next_hop(target_ip);
    if (!next_hop) return -ENETUNREACH;
    rc = ipv4_send_payload(cfg->ip,
                           target_ip,
                           next_hop,
                           IPPROTO_UDP,
                           packet,
                           len + 8u);
    return rc < 0 ? rc : (int)len;
}

static int sys_udp_recv_socket(process_t* proc,
                               fd_entry_t* ent,
                               void* buf,
                               unsigned int len,
                               unsigned int flags,
                               unsigned int* out_src_ip,
                               unsigned int* out_src_port) {
    if (!proc || !ent || (!buf && len != 0u) || socket_kind(ent->socket) != SOCKET_KIND_UDP) {
        return -EINVAL;
    }

    if (!socket_udp_recv_ready(ent->socket) &&
        ((ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u ||
         (flags & MSG_DONTWAIT) != 0u)) {
        return -EAGAIN;
    }

    while (!socket_udp_recv_ready(ent->socket)) {
        int wait_rc;
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_udp_recv_ready(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);

    return socket_udp_recv(ent->socket,
                           buf,
                           len,
                           out_src_ip,
                           out_src_port,
                           (flags & MSG_PEEK) != 0u);
}

int sys_recv_impl(syscall_regs_t* regs, int fd, void* buf, unsigned int len) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    int rc;

    if (!proc) return -EINVAL;
    if (len == 0u) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_kind(ent->socket) == SOCKET_KIND_NETLINK_ROUTE) {
        return sys_netlink_recv_user(proc, ent, buf, len, 0u, 0, 0);
    }
    if (socket_kind(ent->socket) == SOCKET_KIND_RAW_ICMP) {
        (void)regs;
        return sys_raw_icmp_recv_socket(proc, ent, buf, len, 0u, 0);
    }
    if (socket_kind(ent->socket) == SOCKET_KIND_UDP) {
        (void)regs;
        return sys_udp_recv_socket(proc, ent, buf, len, 0u, 0, 0);
    }
    if (socket_state(ent->socket) != SOCKET_STATE_CONNECTED &&
        socket_state(ent->socket) != SOCKET_STATE_CONNECTING) return -EINVAL;

    if (!socket_tcp_recv_ready(ent->socket) &&
        (ent->flags & SYS_FD_FLAG_NONBLOCK) != 0u) {
        return -EAGAIN;
    }

    while (!socket_tcp_recv_ready(ent->socket)) {
        int wait_rc;

        if (!socket_tcp_connection_established(ent->socket)) {
            if (!socket_tcp_connect_pending(ent->socket)) {
                return 0;
            }
            proc->state = PROCESS_STATE_WAITING;
            wait_rc = socket_wait(ent->socket, proc, POLLOUT);
            if (wait_rc < 0) {
                proc->state = PROCESS_STATE_RUNNING;
                socket_wait_clear_process(proc);
                return wait_rc;
            }
            (void)regs;
            sys_wait_until_current_running(proc);
            socket_wait_clear_process(proc);
            continue;
        }
        proc->state = PROCESS_STATE_WAITING;
        wait_rc = socket_wait(ent->socket, proc, POLLIN);
        if (wait_rc < 0) {
            proc->state = PROCESS_STATE_RUNNING;
            socket_wait_clear_process(proc);
            return wait_rc;
        }
        if (socket_tcp_recv_ready(ent->socket)) {
            proc->state = PROCESS_STATE_RUNNING;
            break;
        }
        (void)regs;
        sys_wait_until_current_running(proc);
        socket_wait_clear_process(proc);
    }
    socket_wait_clear_process(proc);

    rc = socket_tcp_recv(ent->socket, buf, len);
    return rc < 0 ? -ECONNRESET : rc;
}

int sys_sendto_impl(int fd,
                           const void* buf,
                           unsigned int len,
                           unsigned int flags,
                           const struct sockaddr* dest_addr,
                           unsigned int addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    if (len == 0u) return 0;
    if (!user_buf_ok((unsigned int)buf, len)) return -EFAULT;
    if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL)) != 0u) return -EINVAL;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;

    if (socket_kind(ent->socket) == SOCKET_KIND_NETLINK_ROUTE) {
        if (dest_addr) {
            struct sockaddr_nl nl;
            if (addrlen < sizeof(nl)) return -EINVAL;
            if (!user_buf_ok((unsigned int)dest_addr, sizeof(nl))) return -EFAULT;
            if (copy_from_user(&nl, dest_addr, sizeof(nl)) < 0) return -EFAULT;
            if (nl.nl_family != AF_NETLINK) return -EINVAL;
        }
        return sys_netlink_send_user(ent, buf, len);
    }

    if (socket_kind(ent->socket) == SOCKET_KIND_TCP) {
        if (dest_addr) return -EISCONN;
        return process_fd_write(ent, (const char*)buf, len);
    }

    if (socket_kind(ent->socket) == SOCKET_KIND_RAW_ICMP) {
        struct sockaddr_in sa;
        const net_ipv4_config_t* cfg = net_ipv4_config();
        unsigned int target_ip;
        unsigned int next_hop;
        unsigned char packet[1480];
        int rc = copy_user_sockaddr_in(&sa, dest_addr, addrlen);
        if (rc < 0) return rc;
        if (!cfg || !cfg->configured) return -ENETUNREACH;
        if (len > sizeof(packet)) return -EMSGSIZE;
        if (copy_from_user(packet, buf, len) < 0) return -EFAULT;
        target_ip = swap_u32(sa.sin_addr.s_addr);
        next_hop = net_route_next_hop(target_ip);
        if (!next_hop) return -ENETUNREACH;
        rc = ipv4_send_payload(cfg->ip,
                               target_ip,
                               next_hop,
                               IPPROTO_ICMP,
                               packet,
                               len);
        return rc < 0 ? rc : (int)len;
    }

    if (socket_kind(ent->socket) == SOCKET_KIND_UDP) {
        return sys_udp_send_socket(ent, buf, len, dest_addr, addrlen);
    }

    return -EOPNOTSUPP;
}

int sys_recvfrom_impl(syscall_regs_t* regs,
                             int fd,
                             void* buf,
                             unsigned int len,
                             unsigned int flags,
                             struct sockaddr* src_addr,
                             unsigned int* addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    if ((flags & ~(MSG_DONTWAIT | MSG_NOSIGNAL | MSG_PEEK)) != 0u) return -EINVAL;

    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (len == 0u &&
        !(socket_kind(ent->socket) == SOCKET_KIND_UDP &&
          (flags & MSG_PEEK) != 0u &&
          src_addr &&
          addrlen)) {
        return 0;
    }
    if (len != 0u && !user_buf_ok((unsigned int)buf, len)) return -EFAULT;

    if (socket_kind(ent->socket) == SOCKET_KIND_NETLINK_ROUTE) {
        return sys_netlink_recv_user(proc, ent, buf, len, flags, src_addr, addrlen);
    }

    if (socket_kind(ent->socket) == SOCKET_KIND_TCP) {
        int rc;
        if (src_addr || addrlen) return -EISCONN;
        rc = sys_recv_impl(regs, fd, buf, len);
        return rc;
    }

    if (socket_kind(ent->socket) == SOCKET_KIND_RAW_ICMP) {
        unsigned int src_ip = 0u;
        int rc;

        rc = sys_raw_icmp_recv_socket(proc, ent, buf, len, flags, &src_ip);
        if (rc < 0) return rc;
        if (src_addr && addrlen) {
            struct sockaddr_in sa;
            unsigned int user_addrlen = 0u;
            if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
            if (user_addrlen < sizeof(sa)) return -EINVAL;
            if (!user_buf_ok((unsigned int)src_addr, sizeof(sa))) return -EFAULT;
            k_memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_addr.s_addr = swap_u32(src_ip);
            if (copy_to_user(src_addr, &sa, sizeof(sa)) < 0 ||
                write_user_u32(addrlen, sizeof(sa)) < 0) {
                return -EFAULT;
            }
        }
        return rc;
    }

    if (socket_kind(ent->socket) == SOCKET_KIND_UDP) {
        unsigned int src_ip = 0u;
        unsigned int src_port = 0u;
        int rc = sys_udp_recv_socket(proc, ent, buf, len, flags, &src_ip, &src_port);
        if (rc < 0) return rc;
        if (src_addr && addrlen) {
            struct sockaddr_in sa;
            unsigned int user_addrlen = 0u;
            if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
            if (user_addrlen < sizeof(sa)) return -EINVAL;
            if (!user_buf_ok((unsigned int)src_addr, sizeof(sa))) return -EFAULT;
            k_memset(&sa, 0, sizeof(sa));
            sa.sin_family = AF_INET;
            sa.sin_port = swap_u16((unsigned short)src_port);
            sa.sin_addr.s_addr = swap_u32(src_ip);
            if (copy_to_user(src_addr, &sa, sizeof(sa)) < 0 ||
                write_user_u32(addrlen, sizeof(sa)) < 0) {
                return -EFAULT;
            }
        } else if (src_addr || addrlen) {
            return -EFAULT;
        }
        return rc;
    }

    if (src_addr || addrlen) {
        if (!src_addr || !addrlen) return -EFAULT;
        if (!user_buf_ok((unsigned int)addrlen, sizeof(*addrlen))) return -EFAULT;
    }
    return (flags & MSG_DONTWAIT) != 0u ? -EAGAIN : -EOPNOTSUPP;
}


int sys_shutdown_impl(int fd, int how) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    return socket_shutdown_tcp(ent->socket, how);
}

int sys_getpeername_impl(int fd, struct sockaddr* addr, unsigned int* addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (socket_state(ent->socket) != SOCKET_STATE_CONNECTED) return -EINVAL;
    if (!addr || !addrlen) return -EFAULT;
    {
        unsigned int user_addrlen = 0;
        if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
        if (user_addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    }
    if (!user_buf_ok((unsigned int)addr, sizeof(struct sockaddr_in))) return -EFAULT;

    k_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = swap_u16((unsigned short)socket_peer_port(ent->socket));
    sa.sin_addr.s_addr = swap_u32(socket_peer_ip(ent->socket));
    if (copy_to_user(addr, &sa, sizeof(sa)) < 0 ||
        write_user_u32(addrlen, sizeof(sa)) < 0) {
        return -EFAULT;
    }
    return 0;
}


int sys_setsockopt_impl(int fd, int level, int optname) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (level == SOL_SOCKET && optname == SO_REUSEADDR) {
        socket_set_reuseaddr(ent->socket, 1);
        return 0;
    }
    if (level == SOL_SOCKET && optname == SO_BROADCAST) {
        return 0;
    }
    return 0;
}

int sys_getsockname_impl(int fd, struct sockaddr* addr, unsigned int* addrlen) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;
    struct sockaddr_in sa;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;
    if (!addr || !addrlen) return -EFAULT;
    if (socket_kind(ent->socket) == SOCKET_KIND_NETLINK_ROUTE) {
        struct sockaddr_nl nl;
        unsigned int user_addrlen = 0;
        if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
        if (user_addrlen < sizeof(nl)) return -EINVAL;
        if (!user_buf_ok((unsigned int)addr, sizeof(nl))) return -EFAULT;
        k_memset(&nl, 0, sizeof(nl));
        nl.nl_family = AF_NETLINK;
        nl.nl_pid = socket_netlink_pid(ent->socket);
        nl.nl_groups = socket_netlink_groups(ent->socket);
        if (copy_to_user(addr, &nl, sizeof(nl)) < 0 ||
            write_user_u32(addrlen, sizeof(nl)) < 0) {
            return -EFAULT;
        }
        return 0;
    }
    {
        unsigned int user_addrlen = 0;
        if (read_user_u32(&user_addrlen, addrlen) < 0) return -EFAULT;
        if (user_addrlen < sizeof(struct sockaddr_in)) return -EINVAL;
    }
    if (!user_buf_ok((unsigned int)addr, sizeof(struct sockaddr_in))) return -EFAULT;

    k_memset(&sa, 0, sizeof(sa));
    sa.sin_family = AF_INET;
    sa.sin_port = swap_u16((unsigned short)socket_local_port(ent->socket));
    sa.sin_addr.s_addr = swap_u32(socket_local_ip(ent->socket));
    if (copy_to_user(addr, &sa, sizeof(sa)) < 0 ||
        write_user_u32(addrlen, sizeof(sa)) < 0) {
        return -EFAULT;
    }
    return 0;
}

static int net_ifreq_name_to_index(const char* name) {
    if (!name) return 0;
    if (k_strcmp(name, "eth0")) return 1;
    if (k_strcmp(name, "lo")) return 2;
    return 0;
}

static void net_set_sockaddr_in(struct sockaddr* out, unsigned int ip) {
    struct sockaddr_in* in = (struct sockaddr_in*)out;
    k_memset(out, 0, sizeof(*out));
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = swap_u32(ip);
}

static unsigned int net_sockaddr_ip(const struct sockaddr* addr) {
    const struct sockaddr_in* in = (const struct sockaddr_in*)addr;
    if (!addr || addr->sa_family != AF_INET) return 0u;
    return swap_u32(in->sin_addr.s_addr);
}

static unsigned int net_route_next_hop(unsigned int target_ip) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    if (!cfg || !cfg->configured) return 0u;
    if (cfg->netmask != 0u &&
        (target_ip & cfg->netmask) == (cfg->ip & cfg->netmask)) {
        return target_ip;
    }
    return cfg->gateway ? cfg->gateway : target_ip;
}

static void net_write_u16_be(unsigned char* buf, unsigned int off, unsigned int value) {
    buf[off] = (unsigned char)((value >> 8) & 0xFFu);
    buf[off + 1u] = (unsigned char)(value & 0xFFu);
}

static void net_fill_eth0_ifreq(struct ifreq* ifr, unsigned int request) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    const u8* mac = nic_mac();
    unsigned int flags = IFF_BROADCAST | IFF_MULTICAST;

    k_memset(ifr, 0, sizeof(*ifr));
    k_memcpy(ifr->ifr_name, "eth0", 5u);
    if (cfg && cfg->configured) flags |= IFF_UP;
    if (nic_link_up()) flags |= IFF_RUNNING;

    switch (request) {
    case SIOCGIFFLAGS:
        ifr->ifr_flags = (short)flags;
        break;
    case SIOCGIFADDR:
        net_set_sockaddr_in(&ifr->ifr_addr, cfg && cfg->configured ? cfg->ip : 0u);
        break;
    case SIOCGIFDSTADDR:
        net_set_sockaddr_in(&ifr->ifr_dstaddr, cfg ? cfg->gateway : 0u);
        break;
    case SIOCGIFBRDADDR:
        net_set_sockaddr_in(&ifr->ifr_broadaddr,
                            cfg && cfg->configured
                              ? ((cfg->ip & cfg->netmask) | (~cfg->netmask))
                              : 0u);
        break;
    case SIOCGIFNETMASK:
        net_set_sockaddr_in(&ifr->ifr_netmask,
                            cfg && cfg->configured ? cfg->netmask : 0u);
        break;
    case SIOCGIFHWADDR:
        k_memset(&ifr->ifr_hwaddr, 0, sizeof(ifr->ifr_hwaddr));
        ifr->ifr_hwaddr.sa_family = ARPHRD_ETHER;
        if (mac) k_memcpy(ifr->ifr_hwaddr.sa_data, mac, 6u);
        break;
    case SIOCGIFMTU:
        ifr->ifr_mtu = 1500;
        break;
    case SIOCGIFINDEX:
        ifr->ifr_ifindex = 1;
        break;
    case SIOCGIFMETRIC:
        ifr->ifr_metric = 0;
        break;
    case SIOCGIFTXQLEN:
        ifr->ifr_qlen = 1000;
        break;
    case SIOCGIFNAME:
        break;
    default:
        break;
    }
}

static void net_fill_loopback_ifreq(struct ifreq* ifr, unsigned int request) {
    k_memset(ifr, 0, sizeof(*ifr));
    k_memcpy(ifr->ifr_name, "lo", 3u);

    switch (request) {
    case SIOCGIFFLAGS:
        ifr->ifr_flags = (short)net_loopback_flags();
        break;
    case SIOCGIFADDR:
        net_set_sockaddr_in(&ifr->ifr_addr, 0x7F000001u);
        break;
    case SIOCGIFDSTADDR:
        net_set_sockaddr_in(&ifr->ifr_dstaddr, 0x7F000001u);
        break;
    case SIOCGIFBRDADDR:
        net_set_sockaddr_in(&ifr->ifr_broadaddr, 0u);
        break;
    case SIOCGIFNETMASK:
        net_set_sockaddr_in(&ifr->ifr_netmask, 0xFF000000u);
        break;
    case SIOCGIFHWADDR:
        k_memset(&ifr->ifr_hwaddr, 0, sizeof(ifr->ifr_hwaddr));
        ifr->ifr_hwaddr.sa_family = ARPHRD_LOOPBACK;
        break;
    case SIOCGIFMTU:
        ifr->ifr_mtu = 65536;
        break;
    case SIOCGIFINDEX:
        ifr->ifr_ifindex = 2;
        break;
    case SIOCGIFMETRIC:
        ifr->ifr_metric = 0;
        break;
    case SIOCGIFTXQLEN:
        ifr->ifr_qlen = 1000;
        break;
    case SIOCGIFNAME:
        break;
    default:
        break;
    }
}

static void net_fill_ifreq_by_index(struct ifreq* ifr,
                                    unsigned int request,
                                    int ifindex) {
    if (ifindex == 2) {
        net_fill_loopback_ifreq(ifr, request);
    } else {
        net_fill_eth0_ifreq(ifr, request);
    }
}

static int net_apply_ifreq_setter(unsigned int request, const struct ifreq* ifr) {
    const net_ipv4_config_t* cfg = net_ipv4_config();
    unsigned int ip = cfg ? cfg->ip : 0u;
    unsigned int netmask = cfg && cfg->netmask ? cfg->netmask : 0xFFFFFF00u;
    unsigned int gateway = cfg ? cfg->gateway : 0u;
    unsigned int dns = cfg ? cfg->dns : 0u;
    unsigned int dhcp_server = cfg ? cfg->dhcp_server : 0u;
    unsigned int lease_seconds = cfg ? cfg->lease_seconds : 0u;

    switch (request) {
    case SIOCSIFFLAGS:
    case SIOCSIFBRDADDR:
    case SIOCSIFDSTADDR:
    case SIOCSIFMETRIC:
    case SIOCSIFMTU:
    case SIOCSIFTXQLEN:
        return 0;
    case SIOCSIFADDR:
        ip = net_sockaddr_ip(&ifr->ifr_addr);
        if (ip == 0u) return -EINVAL;
        break;
    case SIOCSIFNETMASK:
        netmask = net_sockaddr_ip(&ifr->ifr_netmask);
        if (netmask == 0u) return -EINVAL;
        break;
    default:
        return -ENOTTY;
    }

    net_ipv4_configure(ip, netmask, gateway, dns, dhcp_server, lease_seconds);
    return 0;
}

static int net_apply_loopback_setter(unsigned int request, const struct ifreq* ifr) {
    unsigned int ip;
    unsigned int mask;

    switch (request) {
    case SIOCSIFFLAGS:
    case SIOCSIFBRDADDR:
    case SIOCSIFDSTADDR:
    case SIOCSIFMETRIC:
    case SIOCSIFMTU:
    case SIOCSIFTXQLEN:
        return 0;
    case SIOCSIFADDR:
        ip = net_sockaddr_ip(&ifr->ifr_addr);
        return ip == 0x7F000001u ? 0 : -EADDRNOTAVAIL;
    case SIOCSIFNETMASK:
        mask = net_sockaddr_ip(&ifr->ifr_netmask);
        return mask == 0xFF000000u ? 0 : -EINVAL;
    default:
        return -ENOTTY;
    }
}

static int net_ioctl_ifconf(void* argp) {
    struct ifconf ifc;
    struct ifreq ifrs[2];
    unsigned int need_len = (unsigned int)sizeof(ifrs);
    unsigned int copy_len;

    if (!argp) return -EFAULT;
    if (!user_buf_ok((unsigned int)argp, sizeof(ifc))) return -EFAULT;
    if (copy_from_user(&ifc, argp, sizeof(ifc)) < 0) return -EFAULT;

    if (ifc.ifc_len > 0 && ifc.ifc_buf) {
        copy_len = (unsigned int)ifc.ifc_len;
        if (copy_len > need_len) copy_len = need_len;
        if (!user_buf_ok((unsigned int)ifc.ifc_buf, copy_len)) return -EFAULT;
        net_fill_eth0_ifreq(&ifrs[0], SIOCGIFADDR);
        net_fill_loopback_ifreq(&ifrs[1], SIOCGIFADDR);
        if (copy_to_user(ifc.ifc_buf, ifrs, copy_len) < 0) return -EFAULT;
    }
    ifc.ifc_len = (int)need_len;
    if (copy_to_user(argp, &ifc, sizeof(ifc)) < 0) return -EFAULT;
    return 0;
}

static int net_ioctl_ifreq(unsigned int request, void* argp) {
    struct ifreq ifr;
    int ifindex;

    if (!argp) return -EFAULT;
    if (!user_buf_ok((unsigned int)argp, sizeof(ifr))) return -EFAULT;
    if (copy_from_user(&ifr, argp, sizeof(ifr)) < 0) return -EFAULT;

    if (request == SIOCGIFNAME) {
        if (ifr.ifr_ifindex != 1 && ifr.ifr_ifindex != 2) return -ENODEV;
        net_fill_ifreq_by_index(&ifr, request, ifr.ifr_ifindex);
        return copy_to_user(argp, &ifr, sizeof(ifr)) < 0 ? -EFAULT : 0;
    }

    ifindex = net_ifreq_name_to_index(ifr.ifr_name);
    if (ifindex == 0) return -ENODEV;
    switch (request) {
    case SIOCGIFFLAGS:
    case SIOCGIFADDR:
    case SIOCGIFDSTADDR:
    case SIOCGIFBRDADDR:
    case SIOCGIFNETMASK:
    case SIOCGIFHWADDR:
    case SIOCGIFMTU:
    case SIOCGIFMETRIC:
    case SIOCGIFTXQLEN:
    case SIOCGIFINDEX:
        net_fill_ifreq_by_index(&ifr, request, ifindex);
        return copy_to_user(argp, &ifr, sizeof(ifr)) < 0 ? -EFAULT : 0;
    case SIOCSIFFLAGS:
    case SIOCSIFADDR:
    case SIOCSIFDSTADDR:
    case SIOCSIFBRDADDR:
    case SIOCSIFNETMASK:
    case SIOCSIFMETRIC:
    case SIOCSIFMTU:
    case SIOCSIFTXQLEN:
        return ifindex == 2
                 ? net_apply_loopback_setter(request, &ifr)
                 : net_apply_ifreq_setter(request, &ifr);
    default:
        return -ENOTTY;
    }
}

static int net_ioctl_route(unsigned int request, void* argp) {
    struct rtentry rt;
    const net_ipv4_config_t* cfg;
    unsigned int dst;
    unsigned int gateway;
    unsigned int netmask;

    if (!argp) return -EFAULT;
    if (!user_buf_ok((unsigned int)argp, sizeof(rt))) return -EFAULT;
    if (copy_from_user(&rt, argp, sizeof(rt)) < 0) return -EFAULT;

    cfg = net_ipv4_config();
    if (!cfg || !cfg->configured) return -ENETUNREACH;

    dst = net_sockaddr_ip(&rt.rt_dst);
    gateway = net_sockaddr_ip(&rt.rt_gateway);
    netmask = net_sockaddr_ip(&rt.rt_genmask);

    if (request == SIOCADDRT) {
        if ((rt.rt_flags & RTF_GATEWAY) == 0u) return 0;
        if (dst != 0u || gateway == 0u) return -EINVAL;
        net_ipv4_configure(cfg->ip,
                           cfg->netmask,
                           gateway,
                           cfg->dns,
                           cfg->dhcp_server,
                           cfg->lease_seconds);
        return 0;
    }

    if (request == SIOCDELRT) {
        if (dst != 0u || (netmask != 0u && netmask != 0xFFFFFFFFu)) return -EINVAL;
        net_ipv4_configure(cfg->ip,
                           cfg->netmask,
                           0u,
                           cfg->dns,
                           cfg->dhcp_server,
                           cfg->lease_seconds);
        return 0;
    }
    return -ENOTTY;
}

int sys_net_ioctl_impl(int fd, unsigned int request, void* argp) {
    process_t* proc = (process_t*)sched_current();
    fd_entry_t* ent;

    if (!proc) return -EINVAL;
    ent = process_fd_get(proc, fd);
    if (!socket_fd_is_socket(ent)) return -EBADF;

    if (request == SIOCGIFCONF) return net_ioctl_ifconf(argp);
    if (request == SIOCADDRT || request == SIOCDELRT) {
        return net_ioctl_route(request, argp);
    }
    if ((request >= SIOCGIFNAME && request <= SIOCGIFHWADDR) ||
        request == SIOCGIFTXQLEN || request == SIOCSIFTXQLEN ||
        request == SIOCGIFINDEX) {
        return net_ioctl_ifreq(request, argp);
    }
    return -ENOTTY;
}


int sys_netinfo_impl(sys_netinfo_t* out_info) {
    sys_netinfo_t info;
    const u8* mac;
    const net_ipv4_config_t* cfg;
    nic_stats_t nic_stats;
    socket_stats_t socket_stats;
    tcp_stats_t tcp_stats;

    if (!out_info) return -EFAULT;
    k_memset(&info, 0, sizeof(info));

    info.net_link_up = nic_link_up() ? 1u : 0u;
    k_strncpy(info.net_driver, nic_driver_name(), sizeof(info.net_driver));
    mac = nic_mac();
    if (mac) {
        for (unsigned int i = 0; i < 6u; i++) info.mac[i] = mac[i];
    }
    nic_get_stats(&nic_stats);
    info.nic_tx_packets = nic_stats.tx_packets;
    info.nic_rx_packets = nic_stats.rx_packets;
    info.nic_tx_errors = nic_stats.tx_errors;
    info.nic_rx_errors = nic_stats.rx_errors;
    info.nic_status = nic_stats.status;
    info.nic_command = nic_stats.command;
    info.nic_rx_config = nic_stats.rx_config;
    info.nic_tx_config = nic_stats.tx_config;
    info.nic_rx_cursor = nic_stats.rx_cursor;
    info.nic_rx_hw_cursor = nic_stats.rx_hw_cursor;

    cfg = net_ipv4_config();
    if (cfg) {
        info.ipv4_configured = cfg->configured ? 1u : 0u;
        info.ip = cfg->ip;
        info.netmask = cfg->netmask;
        info.gateway = cfg->gateway;
        info.dns = cfg->dns;
        info.dhcp_server = cfg->dhcp_server;
        info.lease_seconds = cfg->lease_seconds;
    }

    socket_get_stats(&socket_stats);
    info.max_sockets = socket_stats.max_sockets;
    info.used_sockets = socket_stats.used_sockets;
    info.tcp_sockets = socket_stats.tcp_sockets;
    info.open_sockets = socket_stats.open_sockets;
    info.bound_sockets = socket_stats.bound_sockets;
    info.listening_sockets = socket_stats.listening_sockets;
    info.connected_sockets = socket_stats.connected_sockets;

    tcp_get_stats(&tcp_stats);
    info.tcp_listeners = tcp_stats.listeners;
    info.tcp_max_listeners = tcp_stats.max_listeners;
    info.tcp_connections = tcp_stats.connections;
    info.tcp_max_connections = tcp_stats.max_connections;
    info.tcp_established_connections = tcp_stats.established_connections;
    info.tcp_accepted_connections = tcp_stats.accepted_connections;
    info.tcp_pending_connections = tcp_stats.pending_connections;
    info.tcp_syn_recv_connections = tcp_stats.syn_recv_connections;
    info.tcp_fin_wait_connections = tcp_stats.fin_wait_connections;
    info.tcp_rx_rings = tcp_stats.rx_rings;
    info.tcp_tx_rings = tcp_stats.tx_rings;
    info.tcp_rx_bytes = tcp_stats.rx_bytes;
    info.tcp_tx_bytes = tcp_stats.tx_bytes;
    info.tcp_rx_buffer_bytes = tcp_stats.rx_buffer_bytes;
    info.tcp_tx_buffer_bytes = tcp_stats.tx_buffer_bytes;
    info.tcp_max_rx_buffer_bytes = tcp_stats.max_rx_buffer_bytes;
    info.tcp_max_tx_buffer_bytes = tcp_stats.max_tx_buffer_bytes;

    if (copy_to_user(out_info, &info, sizeof(info)) < 0) return -EFAULT;
    return 0;
}

static int sys_net_route_for_target(u32 target_ip, u32* out_sender_ip, u32* out_next_hop) {
    u32 sender_ip = net_ipv4_local_ip();
    u32 netmask = net_ipv4_netmask();
    u32 gateway = net_ipv4_gateway();
    u32 next_hop = target_ip;

    if (!net_ipv4_is_configured() || sender_ip == 0u) return -ENETUNREACH;
    if (netmask != 0u && (target_ip & netmask) != (sender_ip & netmask)) {
        if (gateway == 0u) return -ENETUNREACH;
        next_hop = gateway;
    }

    if (out_sender_ip) *out_sender_ip = sender_ip;
    if (out_next_hop) *out_next_hop = next_hop;
    return 0;
}

int sys_net_op_impl(sys_net_op_request_t* user_req) {
    sys_net_op_request_t req;
    int rc;

    if (!user_req) return -EFAULT;
    if (copy_from_user(&req, user_req, sizeof(req)) < 0) return -EFAULT;

    switch (req.op) {
    case SYS_NET_OP_SEND_TEST_FRAME:
        return nic_send_test_frame() ? 1 : -EIO;
    case SYS_NET_OP_POLL_ONCE:
        return net_poll_once() ? 1 : 0;
    case SYS_NET_OP_DHCP:
        return dhcp_configure() ? 1 : 0;
    case SYS_NET_OP_CONFIGURE:
        if (req.target_ip == 0u) return -EINVAL;
        net_ipv4_configure(req.target_ip, req.netmask, req.gateway, req.dns,
                           req.dhcp_server, req.lease_seconds);
        return 1;
    case SYS_NET_OP_CLEAR_CONFIG:
        net_ipv4_clear_config();
        return 1;
    case SYS_NET_OP_ARP:
        if (req.target_ip == 0u) req.target_ip = net_ipv4_gateway();
        rc = sys_net_route_for_target(req.target_ip, &req.sender_ip, &req.next_hop_ip);
        if (rc < 0) return rc;
        if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
        if (!arp_resolve(req.sender_ip, req.next_hop_ip, req.mac)) return 0;
        if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
        return 1;
    case SYS_NET_OP_PING:
        if (req.target_ip == 0u) req.target_ip = net_ipv4_gateway();
        rc = sys_net_route_for_target(req.target_ip, &req.sender_ip, &req.next_hop_ip);
        if (rc < 0) return rc;
        if (copy_to_user(user_req, &req, sizeof(req)) < 0) return -EFAULT;
        return ipv4_ping_via_gateway(req.sender_ip, req.target_ip, req.next_hop_ip) ? 1 : 0;
    default:
        return -EINVAL;
    }
}
