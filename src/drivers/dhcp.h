#ifndef DHCP_H
#define DHCP_H

#include "../kernel/types.h"

typedef void (*dhcp_log_hook_t)(const char* text, u32 ip, int has_ip);

int dhcp_configure(void);
void dhcp_set_verbose(int verbose);
void dhcp_set_log_hook(dhcp_log_hook_t hook);
int dhcp_handle_ipv4_frame(const u8* frame, u32 len);

#endif /* DHCP_H */
