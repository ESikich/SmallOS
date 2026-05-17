#ifndef RTL8139_H
#define RTL8139_H

#include "nic.h"

int rtl8139_init(void);
void rtl8139_print_info(void);
int rtl8139_link_up(void);
const u8* rtl8139_mac(void);
void rtl8139_get_stats(nic_stats_t* out);
int rtl8139_send(const void* data, u32 len);
int rtl8139_recv(void* out, u32 out_size, u32* out_len);
int rtl8139_send_test_frame(void);

#endif /* RTL8139_H */
