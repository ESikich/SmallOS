#ifndef ARP_H
#define ARP_H

#include "../kernel/types.h"

int arp_resolve(u32 sender_ip, u32 target_ip, u8* out_mac);
int arp_handle_frame(const u8* frame, u32 len);
int arp_lookup(u32 sender_ip, u32 target_ip, u8* out_mac);
void arp_print_ip(u32 ip);

#endif /* ARP_H */
