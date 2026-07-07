#ifndef UAPI_NETLINK_H
#define UAPI_NETLINK_H

#include "uapi_socket.h"

struct sockaddr_nl {
    sa_family_t nl_family;
    unsigned short nl_pad;
    unsigned int nl_pid;
    unsigned int nl_groups;
};

struct nlmsghdr {
    unsigned int nlmsg_len;
    unsigned short nlmsg_type;
    unsigned short nlmsg_flags;
    unsigned int nlmsg_seq;
    unsigned int nlmsg_pid;
};

struct nlmsgerr {
    int error;
    struct nlmsghdr msg;
};

struct rtattr {
    unsigned short rta_len;
    unsigned short rta_type;
};

struct rtgenmsg {
    unsigned char rtgen_family;
};

struct ifinfomsg {
    unsigned char ifi_family;
    unsigned char __ifi_pad;
    unsigned short ifi_type;
    int ifi_index;
    unsigned int ifi_flags;
    unsigned int ifi_change;
};

struct ifaddrmsg {
    unsigned char ifa_family;
    unsigned char ifa_prefixlen;
    unsigned char ifa_flags;
    unsigned char ifa_scope;
    unsigned int ifa_index;
};

struct rtmsg {
    unsigned char rtm_family;
    unsigned char rtm_dst_len;
    unsigned char rtm_src_len;
    unsigned char rtm_tos;
    unsigned char rtm_table;
    unsigned char rtm_protocol;
    unsigned char rtm_scope;
    unsigned char rtm_type;
    unsigned int rtm_flags;
};

struct ndmsg {
    unsigned char ndm_family;
    unsigned char ndm_pad1;
    unsigned short ndm_pad2;
    int ndm_ifindex;
    unsigned short ndm_state;
    unsigned char ndm_flags;
    unsigned char ndm_type;
};

#define NLMSG_ALIGNTO 4u
#define NLMSG_ALIGN(len) (((len) + NLMSG_ALIGNTO - 1u) & ~(NLMSG_ALIGNTO - 1u))
#define NLMSG_HDRLEN ((unsigned int)NLMSG_ALIGN(sizeof(struct nlmsghdr)))
#define NLMSG_LENGTH(len) ((unsigned int)((len) + NLMSG_HDRLEN))
#define NLMSG_SPACE(len) NLMSG_ALIGN(NLMSG_LENGTH(len))
#define NLMSG_DATA(nlh) ((void*)((char*)(nlh) + NLMSG_LENGTH(0)))
#define NLMSG_NEXT(nlh, len) ((len) -= NLMSG_ALIGN((nlh)->nlmsg_len), \
                              (struct nlmsghdr*)(((char*)(nlh)) + NLMSG_ALIGN((nlh)->nlmsg_len)))
#define NLMSG_OK(nlh, len) ((len) >= (int)sizeof(struct nlmsghdr) && \
                            (nlh)->nlmsg_len >= sizeof(struct nlmsghdr) && \
                            (nlh)->nlmsg_len <= (unsigned int)(len))

#define RTA_ALIGNTO 4u
#define RTA_ALIGN(len) (((len) + RTA_ALIGNTO - 1u) & ~(RTA_ALIGNTO - 1u))
#define RTA_LENGTH(len) ((unsigned int)(RTA_ALIGN(sizeof(struct rtattr)) + (len)))
#define RTA_SPACE(len) RTA_ALIGN(RTA_LENGTH(len))
#define RTA_DATA(rta) ((void*)((char*)(rta) + RTA_LENGTH(0)))
#define RTA_PAYLOAD(rta) ((int)((rta)->rta_len - RTA_LENGTH(0)))
#define RTA_NEXT(rta, attrlen) ((attrlen) -= RTA_ALIGN((rta)->rta_len), \
                                (struct rtattr*)(((char*)(rta)) + RTA_ALIGN((rta)->rta_len)))
#define RTA_OK(rta, attrlen) ((attrlen) >= (int)sizeof(struct rtattr) && \
                              (rta)->rta_len >= sizeof(struct rtattr) && \
                              (rta)->rta_len <= (unsigned int)(attrlen))

#define NLMSG_NOOP    1
#define NLMSG_ERROR   2
#define NLMSG_DONE    3
#define NLMSG_OVERRUN 4

#define NLM_F_REQUEST 0x0001u
#define NLM_F_MULTI   0x0002u
#define NLM_F_ACK     0x0004u
#define NLM_F_ECHO    0x0008u
#define NLM_F_ROOT    0x0100u
#define NLM_F_MATCH   0x0200u
#define NLM_F_ATOMIC  0x0400u
#define NLM_F_DUMP    (NLM_F_ROOT | NLM_F_MATCH)
#define NLM_F_REPLACE 0x0100u
#define NLM_F_EXCL    0x0200u
#define NLM_F_CREATE  0x0400u
#define NLM_F_APPEND  0x0800u

#define RTM_BASE      16
#define RTM_NEWLINK   16
#define RTM_DELLINK   17
#define RTM_GETLINK   18
#define RTM_SETLINK   19
#define RTM_NEWADDR   20
#define RTM_DELADDR   21
#define RTM_GETADDR   22
#define RTM_NEWROUTE  24
#define RTM_DELROUTE  25
#define RTM_GETROUTE  26
#define RTM_NEWNEIGH  28
#define RTM_DELNEIGH  29
#define RTM_GETNEIGH  30

#define RTMGRP_LINK   1u
#define RTMGRP_IPV4_IFADDR 0x10u
#define RTMGRP_IPV4_ROUTE  0x40u
#define RTMGRP_NEIGH       0x04u

#define RT_TABLE_UNSPEC 0
#define RT_TABLE_COMPAT 252
#define RT_TABLE_DEFAULT 253
#define RT_TABLE_MAIN 254
#define RT_TABLE_LOCAL 255

#define RTPROT_UNSPEC 0
#define RTPROT_REDIRECT 1
#define RTPROT_KERNEL 2
#define RTPROT_BOOT 3
#define RTPROT_STATIC 4
#define RTPROT_DHCP 16

#define RT_SCOPE_UNIVERSE 0
#define RT_SCOPE_SITE 200
#define RT_SCOPE_LINK 253
#define RT_SCOPE_HOST 254
#define RT_SCOPE_NOWHERE 255

#define RTN_UNSPEC 0
#define RTN_UNICAST 1
#define RTN_LOCAL 2
#define RTN_BROADCAST 3
#define RTN_ANYCAST 4
#define RTN_MULTICAST 5
#define RTN_BLACKHOLE 6
#define RTN_UNREACHABLE 7
#define RTN_PROHIBIT 8
#define RTN_THROW 9
#define RTN_NAT 10
#define RTN_XRESOLVE 11

#define RTM_F_NOTIFY 0x100u
#define RTM_F_CLONED 0x200u

#define RTNH_F_DEAD 1u
#define RTNH_F_PERVASIVE 2u
#define RTNH_F_ONLINK 4u

#define IFLA_UNSPEC 0
#define IFLA_ADDRESS 1
#define IFLA_BROADCAST 2
#define IFLA_IFNAME 3
#define IFLA_MTU 4
#define IFLA_LINK 5
#define IFLA_QDISC 6
#define IFLA_STATS 7
#define IFLA_MASTER 10
#define IFLA_OPERSTATE 16
#define IFLA_LINKINFO 18
#define IFLA_NET_NS_PID 19
#define IFLA_MAX 48

#define IFLA_INFO_UNSPEC 0
#define IFLA_INFO_KIND 1
#define IFLA_INFO_DATA 2
#define IFLA_INFO_XSTATS 3
#define IFLA_INFO_SLAVE_KIND 4
#define IFLA_INFO_SLAVE_DATA 5

#define IFLA_RTA(r) ((struct rtattr*)(((char*)(r)) + NLMSG_ALIGN(sizeof(struct ifinfomsg))))
#define IFLA_PAYLOAD(n) ((int)((n)->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifinfomsg))))

#define IFA_UNSPEC 0
#define IFA_ADDRESS 1
#define IFA_LOCAL 2
#define IFA_LABEL 3
#define IFA_BROADCAST 4
#define IFA_ANYCAST 5
#define IFA_CACHEINFO 6
#define IFA_MULTICAST 7
#define IFA_FLAGS 8
#define IFA_MAX 10

#define IFA_F_SECONDARY 0x01u
#define IFA_F_TENTATIVE 0x40u
#define IFA_F_PERMANENT 0x80u
#define IFA_F_DADFAILED 0x08u
#define IFA_F_DEPRECATED 0x20u
#define IFA_F_NOPREFIXROUTE 0x200u

struct ifa_cacheinfo {
    unsigned int ifa_prefered;
    unsigned int ifa_valid;
    unsigned int cstamp;
    unsigned int tstamp;
};

#define IFA_RTA(r) ((struct rtattr*)(((char*)(r)) + NLMSG_ALIGN(sizeof(struct ifaddrmsg))))
#define IFA_PAYLOAD(n) ((int)((n)->nlmsg_len - NLMSG_LENGTH(sizeof(struct ifaddrmsg))))

#define RTA_UNSPEC 0
#define RTA_DST 1
#define RTA_SRC 2
#define RTA_IIF 3
#define RTA_OIF 4
#define RTA_GATEWAY 5
#define RTA_PRIORITY 6
#define RTA_PREFSRC 7
#define RTA_METRICS 8
#define RTA_FLOW 11
#define RTA_CACHEINFO 12
#define RTA_TABLE 15
#define RTA_MAX 24

struct rta_cacheinfo {
    unsigned int rta_clntref;
    unsigned int rta_lastuse;
    int rta_expires;
    unsigned int rta_error;
    unsigned int rta_used;
    unsigned int rta_id;
    unsigned int rta_ts;
    unsigned int rta_tsage;
};

#define RTAX_UNSPEC 0
#define RTAX_LOCK 1
#define RTAX_MTU 2
#define RTAX_WINDOW 3
#define RTAX_RTT 4
#define RTAX_RTTVAR 5
#define RTAX_SSTHRESH 6
#define RTAX_CWND 7
#define RTAX_ADVMSS 8
#define RTAX_REORDERING 9
#define RTAX_HOPLIMIT 10
#define RTAX_INITCWND 11
#define RTAX_FEATURES 12
#define RTAX_RTO_MIN 13
#define RTAX_INITRWND 14
#define RTAX_QUICKACK 15

#define RTAX_FEATURE_ECN 1u

#define RTM_RTA(r) ((struct rtattr*)(((char*)(r)) + NLMSG_ALIGN(sizeof(struct rtmsg))))
#define RTM_PAYLOAD(n) ((int)((n)->nlmsg_len - NLMSG_LENGTH(sizeof(struct rtmsg))))

#define NDA_UNSPEC 0
#define NDA_DST 1
#define NDA_LLADDR 2
#define NDA_CACHEINFO 3
#define NDA_PROBES 4
#define NDA_VLAN 5
#define NDA_PORT 6
#define NDA_VNI 7
#define NDA_IFINDEX 8
#define NDA_MASTER 9
#define NDA_LINK_NETNSID 10
#define NDA_SRC_VNI 11
#define NDA_PROTOCOL 12
#define NDA_MAX 12

#define NTF_USE 0x01u
#define NTF_SELF 0x02u
#define NTF_MASTER 0x04u
#define NTF_PROXY 0x08u
#define NTF_EXT_LEARNED 0x10u
#define NTF_OFFLOADED 0x20u
#define NTF_ROUTER 0x80u

#define NUD_INCOMPLETE 0x01u
#define NUD_REACHABLE 0x02u
#define NUD_STALE 0x04u
#define NUD_DELAY 0x08u
#define NUD_PROBE 0x10u
#define NUD_FAILED 0x20u
#define NUD_NOARP 0x40u
#define NUD_PERMANENT 0x80u
#define NUD_NONE 0x00u

#define NDA_RTA(r) ((struct rtattr*)(((char*)(r)) + NLMSG_ALIGN(sizeof(struct ndmsg))))
#define NDA_PAYLOAD(n) ((int)((n)->nlmsg_len - NLMSG_LENGTH(sizeof(struct ndmsg))))

struct nda_cacheinfo {
    unsigned int ndm_confirmed;
    unsigned int ndm_used;
    unsigned int ndm_updated;
    unsigned int ndm_refcnt;
};

#endif /* UAPI_NETLINK_H */
