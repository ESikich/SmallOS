#ifndef USER_LINUX_IF_VLAN_H
#define USER_LINUX_IF_VLAN_H

#define VLAN_FLAG_REORDER_HDR 0x1
#define VLAN_FLAG_GVRP 0x2
#define VLAN_FLAG_LOOSE_BINDING 0x4
#define VLAN_FLAG_MVRP 0x8
#define IFLA_VLAN_UNSPEC 0
#define IFLA_VLAN_ID 1
#define IFLA_VLAN_FLAGS 2
#define IFLA_VLAN_PROTOCOL 5
#define IFLA_VLAN_MAX 6

struct ifla_vlan_flags {
    unsigned int flags;
    unsigned int mask;
};

#endif /* USER_LINUX_IF_VLAN_H */
