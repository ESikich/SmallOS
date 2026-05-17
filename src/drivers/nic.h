#ifndef NIC_H
#define NIC_H

#include "../kernel/types.h"

typedef struct nic_stats {
    u32 tx_packets;
    u32 rx_packets;
    u32 tx_errors;
    u32 rx_errors;
    u32 status;
    u32 command;
    u32 rx_config;
    u32 tx_config;
    u32 rx_cursor;
    u32 rx_hw_cursor;
} nic_stats_t;

int nic_init(void);
void nic_print_info(void);
int nic_link_up(void);
const char* nic_driver_name(void);
const u8* nic_mac(void);
void nic_get_stats(nic_stats_t* out);
int nic_send(const void* data, u32 len);
int nic_recv(void* out, u32 out_size, u32* out_len);
int nic_send_test_frame(void);

#endif /* NIC_H */
