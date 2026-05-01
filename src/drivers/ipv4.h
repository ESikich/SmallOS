#ifndef IPV4_H
#define IPV4_H

#include "../kernel/types.h"

int ipv4_ping(u32 sender_ip, u32 target_ip);
void ipv4_print_ip(u32 ip);
int ipv4_parse_ip(const char* text, u32* out_ip);

#endif /* IPV4_H */
