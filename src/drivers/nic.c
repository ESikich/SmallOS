#include "nic.h"

#include "e1000.h"
#include "rtl8139.h"
#include "terminal.h"

typedef struct {
    const char* name;
    void (*print_info)(void);
    int (*link_up)(void);
    const u8* (*mac)(void);
    void (*get_stats)(nic_stats_t* out);
    int (*send)(const void* data, u32 len);
    int (*recv)(void* out, u32 out_size, u32* out_len);
    int (*send_test_frame)(void);
} nic_driver_t;

static const nic_driver_t s_e1000_driver = {
    "e1000",
    e1000_print_info,
    e1000_link_up,
    e1000_mac,
    e1000_get_stats,
    e1000_send,
    e1000_recv,
    e1000_send_test_frame
};

static const nic_driver_t s_rtl8139_driver = {
    "rtl8139",
    rtl8139_print_info,
    rtl8139_link_up,
    rtl8139_mac,
    rtl8139_get_stats,
    rtl8139_send,
    rtl8139_recv,
    rtl8139_send_test_frame
};

static const nic_driver_t* s_driver = 0;

int nic_init(void) {
    if (e1000_init()) {
        s_driver = &s_e1000_driver;
        terminal_puts("nic: using e1000\n");
        return 1;
    }

    if (rtl8139_init()) {
        s_driver = &s_rtl8139_driver;
        terminal_puts("nic: using rtl8139\n");
        return 1;
    }

    terminal_puts("nic: no supported adapter found\n");
    return 0;
}

void nic_print_info(void) {
    if (!s_driver) {
        terminal_puts("nic: not initialized\n");
        return;
    }
    s_driver->print_info();
}

int nic_link_up(void) {
    return s_driver ? s_driver->link_up() : 0;
}

const char* nic_driver_name(void) {
    return s_driver ? s_driver->name : "none";
}

const u8* nic_mac(void) {
    return s_driver ? s_driver->mac() : 0;
}

void nic_get_stats(nic_stats_t* out) {
    if (!out) {
        return;
    }
    if (!s_driver) {
        out->tx_packets = 0;
        out->rx_packets = 0;
        out->tx_errors = 0;
        out->rx_errors = 0;
        out->status = 0;
        out->command = 0;
        out->rx_config = 0;
        out->tx_config = 0;
        out->rx_cursor = 0;
        out->rx_hw_cursor = 0;
        return;
    }
    s_driver->get_stats(out);
}

int nic_send(const void* data, u32 len) {
    return s_driver ? s_driver->send(data, len) : 0;
}

int nic_recv(void* out, u32 out_size, u32* out_len) {
    return s_driver ? s_driver->recv(out, out_size, out_len) : 0;
}

int nic_send_test_frame(void) {
    return s_driver ? s_driver->send_test_frame() : 0;
}
