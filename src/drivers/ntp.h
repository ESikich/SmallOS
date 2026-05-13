#ifndef NTP_H
#define NTP_H

#include "../kernel/types.h"

#define NTP_DEFAULT_SERVER_IP 0x81060F1Cu /* 129.6.15.28, time-a-g.nist.gov */

int ntp_sync(u32 server_ip, u32* out_unix_time);
int ntp_handle_ipv4_frame(const u8* frame, u32 len);

#endif /* NTP_H */
