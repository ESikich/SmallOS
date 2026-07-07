#include "arpa/inet.h"
#include "errno.h"
#include "fcntl.h"
#include "net/if.h"
#include "net/route.h"
#include "netdb.h"
#include "stdio.h"
#include "string.h"
#include "sys/ioctl.h"
#include "sys/socket.h"
#include "linux/netlink.h"
#include "linux/rtnetlink.h"
#include "unistd.h"

static int failures = 0;

static void check(const char* name, int ok) {
    printf("%s: %s\n", name, ok ? "PASS" : "FAIL");
    if (!ok) failures++;
}

static void set_in(struct sockaddr* sa, const char* text) {
    struct sockaddr_in* in = (struct sockaddr_in*)sa;
    memset(sa, 0, sizeof(*sa));
    in->sin_family = AF_INET;
    in->sin_addr.s_addr = inet_addr(text);
}

static int read_contains(const char* path, const char* needle) {
    char buf[768];
    int fd;
    int n;

    fd = open(path, O_RDONLY);
    if (fd < 0) return 0;
    n = read(fd, buf, sizeof(buf) - 1u);
    close(fd);
    if (n < 0) return 0;
    buf[n] = '\0';
    return strstr(buf, needle) != 0;
}

static void parse_rtattrs(struct rtattr* attrs[], int max,
                          struct rtattr* rta, int len) {
    int i;

    for (i = 0; i <= max; i++) attrs[i] = 0;
    while (RTA_OK(rta, len)) {
        if (rta->rta_type <= max) attrs[rta->rta_type] = rta;
        rta = RTA_NEXT(rta, len);
    }
}

static int netlink_dump_has(int request_type, int response_type, int attr_type) {
    int nlfd;
    struct sockaddr_nl sa;
    struct {
        struct nlmsghdr nlh;
        struct rtgenmsg gen;
    } req;
    char buf[4096];
    struct iovec iov;
    struct msghdr msg;
    int seen = 0;

    nlfd = socket(AF_NETLINK, SOCK_RAW, NETLINK_ROUTE);
    if (nlfd < 0) return 0;

    memset(&sa, 0, sizeof(sa));
    sa.nl_family = AF_NETLINK;
    if (bind(nlfd, (struct sockaddr*)&sa, sizeof(sa)) < 0) {
        close(nlfd);
        return 0;
    }

    memset(&req, 0, sizeof(req));
    req.nlh.nlmsg_len = sizeof(req);
    req.nlh.nlmsg_type = request_type;
    req.nlh.nlmsg_flags = NLM_F_REQUEST | NLM_F_ROOT | NLM_F_MATCH;
    req.nlh.nlmsg_seq = 77u + (unsigned int)request_type;
    req.gen.rtgen_family = AF_UNSPEC;
    if (send(nlfd, &req, sizeof(req), 0) != (int)sizeof(req)) {
        close(nlfd);
        return 0;
    }

    memset(&iov, 0, sizeof(iov));
    memset(&msg, 0, sizeof(msg));
    iov.iov_base = buf;
    iov.iov_len = sizeof(buf);
    msg.msg_name = &sa;
    msg.msg_namelen = sizeof(sa);
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;

    while (!seen) {
        int n = recvmsg(nlfd, &msg, 0);
        struct nlmsghdr* h;
        int left;

        if (n <= 0) break;
        left = n;
        h = (struct nlmsghdr*)buf;
        while (NLMSG_OK(h, left)) {
            if (h->nlmsg_type == NLMSG_DONE) {
                close(nlfd);
                return seen;
            }
            if (h->nlmsg_type == response_type) {
                if (response_type == RTM_NEWLINK) {
                    struct ifinfomsg* ifi = (struct ifinfomsg*)NLMSG_DATA(h);
                    struct rtattr* attrs[IFLA_MAX + 1];
                    parse_rtattrs(attrs, IFLA_MAX, IFLA_RTA(ifi), IFLA_PAYLOAD(h));
                    seen = ifi->ifi_index == 1 && attrs[attr_type] != 0;
                } else if (response_type == RTM_NEWADDR) {
                    struct ifaddrmsg* ifa = (struct ifaddrmsg*)NLMSG_DATA(h);
                    struct rtattr* attrs[IFA_MAX + 1];
                    parse_rtattrs(attrs, IFA_MAX, IFA_RTA(ifa), IFA_PAYLOAD(h));
                    seen = ifa->ifa_index == 1 && attrs[attr_type] != 0;
                } else if (response_type == RTM_NEWROUTE) {
                    struct rtmsg* rtm = (struct rtmsg*)NLMSG_DATA(h);
                    struct rtattr* attrs[RTA_MAX + 1];
                    parse_rtattrs(attrs, RTA_MAX, RTM_RTA(rtm), RTM_PAYLOAD(h));
                    seen = rtm->rtm_family == AF_INET && attrs[attr_type] != 0;
                }
            }
            h = NLMSG_NEXT(h, left);
        }
    }

    close(nlfd);
    return seen;
}

int main(void) {
    int fd;
    struct ifreq ifr;
    struct ifreq ifrs[2];
    struct ifconf ifc;
    struct rtentry rt;
    struct hostent* he;
    struct addrinfo* ai = 0;
    struct addrinfo hints;
    struct sockaddr_in udp_addr;
    socklen_t udp_len;
    char udp_buf[8];
    int udpfd;
    char ifname[IFNAMSIZ];

    puts("netbbprobe start");

    fd = socket(AF_INET, SOCK_DGRAM, 0);
    check("socket dgram", fd >= 0);
    if (fd < 0) {
        printf("netbbprobe FAIL\n");
        return 1;
    }

    check("if_nametoindex eth0", if_nametoindex("eth0") == 1u);
    check("if_indextoname eth0", if_indextoname(1u, ifname) == ifname &&
                                  strcmp(ifname, "eth0") == 0);
    errno = 0;
    check("if_nametoindex lo missing", if_nametoindex("lo") == 0u && errno == ENODEV);

    memset(ifrs, 0, sizeof(ifrs));
    memset(&ifc, 0, sizeof(ifc));
    ifc.ifc_len = sizeof(ifrs);
    ifc.ifc_req = ifrs;
    check("ioctl ifconf", ioctl(fd, SIOCGIFCONF, &ifc) == 0 &&
                           ifc.ifc_len == (int)sizeof(struct ifreq) &&
                           strcmp(ifrs[0].ifr_name, "eth0") == 0);

    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "eth0");
    check("ioctl flags", ioctl(fd, SIOCGIFFLAGS, &ifr) == 0 &&
                         (ifr.ifr_flags & IFF_BROADCAST));
    check("ioctl mtu", ioctl(fd, SIOCGIFMTU, &ifr) == 0 && ifr.ifr_mtu == 1500);
    check("ioctl hwaddr", ioctl(fd, SIOCGIFHWADDR, &ifr) == 0 &&
                          ifr.ifr_hwaddr.sa_family == ARPHRD_ETHER);
    check("ioctl ifindex", ioctl(fd, SIOCGIFINDEX, &ifr) == 0 &&
                           ifr.ifr_ifindex == 1);

    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "eth0");
    set_in(&ifr.ifr_addr, "10.0.2.15");
    check("ioctl set addr", ioctl(fd, SIOCSIFADDR, &ifr) == 0);

    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "eth0");
    set_in(&ifr.ifr_netmask, "255.255.255.0");
    check("ioctl set netmask", ioctl(fd, SIOCSIFNETMASK, &ifr) == 0);

    memset(&ifr, 0, sizeof(ifr));
    strcpy(ifr.ifr_name, "eth0");
    check("ioctl get addr", ioctl(fd, SIOCGIFADDR, &ifr) == 0 &&
                            ((struct sockaddr_in*)&ifr.ifr_addr)->sin_addr.s_addr ==
                                inet_addr("10.0.2.15"));
    check("ioctl get netmask", ioctl(fd, SIOCGIFNETMASK, &ifr) == 0 &&
                               ((struct sockaddr_in*)&ifr.ifr_netmask)->sin_addr.s_addr ==
                                   inet_addr("255.255.255.0"));

    memset(&rt, 0, sizeof(rt));
    set_in(&rt.rt_dst, "0.0.0.0");
    set_in(&rt.rt_gateway, "10.0.2.2");
    set_in(&rt.rt_genmask, "0.0.0.0");
    rt.rt_flags = RTF_UP | RTF_GATEWAY;
    rt.rt_dev = "eth0";
    check("ioctl add default route", ioctl(fd, SIOCADDRT, &rt) == 0);
    check("proc net route gateway", read_contains("/proc/net/route", "0202000A"));
    check("ioctl del default route", ioctl(fd, SIOCDELRT, &rt) == 0);

    check("proc net dev eth0", read_contains("/proc/net/dev", "eth0"));
    check("proc net route header", read_contains("/proc/net/route", "Iface"));
    check("netlink link dump", netlink_dump_has(RTM_GETLINK, RTM_NEWLINK,
                                                IFLA_IFNAME));
    check("netlink addr dump", netlink_dump_has(RTM_GETADDR, RTM_NEWADDR,
                                                IFA_LOCAL));
    check("netlink route dump", netlink_dump_has(RTM_GETROUTE, RTM_NEWROUTE,
                                                 RTA_OIF));

    udpfd = socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK, IPPROTO_UDP);
    check("udp socket", udpfd >= 0);
    if (udpfd >= 0) {
        memset(&udp_addr, 0, sizeof(udp_addr));
        udp_addr.sin_family = AF_INET;
        udp_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        udp_addr.sin_port = htons(40530);
        check("udp bind", bind(udpfd, (struct sockaddr*)&udp_addr,
                               sizeof(udp_addr)) == 0);
        udp_len = sizeof(udp_addr);
        memset(&udp_addr, 0, sizeof(udp_addr));
        check("udp getsockname", getsockname(udpfd, (struct sockaddr*)&udp_addr,
                                             &udp_len) == 0 &&
                                  udp_len == sizeof(udp_addr) &&
                                  udp_addr.sin_port == htons(40530));
        errno = 0;
        check("udp recv eagain", recvfrom(udpfd, udp_buf, sizeof(udp_buf),
                                          MSG_DONTWAIT, 0, 0) < 0 &&
                                  errno == EAGAIN);
        close(udpfd);
    }

    he = gethostbyname("localhost");
    check("resolver localhost", he && he->h_addrtype == AF_INET &&
                                *(unsigned int*)he->h_addr == htonl(INADDR_LOOPBACK));
    check("resolver numeric", getaddrinfo("127.0.0.1", "7", 0, &ai) == 0 &&
                              ai && ai->ai_family == AF_INET &&
                              ((struct sockaddr_in*)ai->ai_addr)->sin_port == htons(7));
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_flags = AI_NUMERICHOST;
    check("resolver invalid", getaddrinfo("smallos.invalid", 0, &hints, &ai) ==
                              EAI_NONAME);

    errno = 0;
    close(fd);
    fd = open("/dev/null", O_RDONLY);
    check("net ioctl enotty fd", fd >= 0 &&
                                  ioctl(fd, SIOCGIFCONF, &ifc) < 0 &&
                                  errno == EBADF);
    if (fd >= 0) close(fd);

    printf("netbbprobe %s\n", failures ? "FAIL" : "PASS");
    return failures ? 1 : 0;
}
