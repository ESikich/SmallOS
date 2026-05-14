#ifndef DHCP_H
#define DHCP_H

#include "../kernel/types.h"

int dhcp_configure(void);
int dhcp_handle_ipv4_frame(const u8* frame, u32 len);

#endif /* DHCP_H */
