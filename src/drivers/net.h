#ifndef NET_H
#define NET_H

#include "../kernel/types.h"

typedef struct {
    int configured;
    u32 ip;
    u32 netmask;
    u32 gateway;
    u32 dns;
    u32 dhcp_server;
    u32 lease_seconds;
} net_ipv4_config_t;

int net_poll_once(void);
void net_poll_drain(void);
void net_ipv4_clear_config(void);
void net_ipv4_configure(u32 ip, u32 netmask, u32 gateway, u32 dns, u32 dhcp_server, u32 lease_seconds);
const net_ipv4_config_t* net_ipv4_config(void);
int net_ipv4_is_configured(void);
u32 net_ipv4_local_ip(void);
u32 net_ipv4_netmask(void);
u32 net_ipv4_gateway(void);
u32 net_ipv4_dns(void);
void net_ipv4_print_config(void);

#endif /* NET_H */
