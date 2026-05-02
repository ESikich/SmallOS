#ifndef IPV4_H
#define IPV4_H

#include "../kernel/types.h"

int ipv4_ping(u32 sender_ip, u32 target_ip);
int ipv4_ping_via_gateway(u32 sender_ip, u32 target_ip, u32 gateway_ip);
int ipv4_handle_frame(const u8* frame, u32 len);
void ipv4_print_ip(u32 ip);
int ipv4_parse_ip(const char* text, u32* out_ip);

#endif /* IPV4_H */
