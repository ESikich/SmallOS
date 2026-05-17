#include "diag_util.h"

void _start(int argc, char** argv) {
    sys_netinfo_t info;

    (void)argc;
    (void)argv;

    if (sys_netinfo(&info) < 0) {
        u_puts("netinfo: unavailable\n");
        sys_exit(1);
    }

    u_puts("netinfo:\n");
    u_puts("nic: driver=");
    u_puts(info.net_driver);
    u_puts(" mac=");
    diag_put_mac(info.mac);
    u_puts(" link=");
    u_puts(info.net_link_up ? "up" : "down");
    u_putc('\n');
    u_puts("nic stats: tx=");
    u_put_uint(info.nic_tx_packets);
    u_puts(" rx=");
    u_put_uint(info.nic_rx_packets);
    u_puts(" txerr=");
    u_put_uint(info.nic_tx_errors);
    u_puts(" rxerr=");
    u_put_uint(info.nic_rx_errors);
    u_puts(" status=");
    diag_put_hex32(info.nic_status);
    u_puts(" cmd=");
    diag_put_hex32(info.nic_command);
    u_puts(" rcr=");
    diag_put_hex32(info.nic_rx_config);
    u_puts(" tcr=");
    diag_put_hex32(info.nic_tx_config);
    u_puts(" rxcur=");
    u_put_uint(info.nic_rx_cursor);
    u_puts(" rxhw=");
    u_put_uint(info.nic_rx_hw_cursor);
    u_putc('\n');

    if (info.ipv4_configured) {
        u_puts("net: ip=");
        diag_put_ip(info.ip);
        u_puts(" mask=");
        diag_put_ip(info.netmask);
        u_puts(" gw=");
        diag_put_ip(info.gateway);
        if (info.dns) {
            u_puts(" dns=");
            diag_put_ip(info.dns);
        }
        if (info.lease_seconds) {
            u_puts(" lease=");
            u_put_uint(info.lease_seconds);
        }
        u_putc('\n');
    } else {
        u_puts("net: IPv4 unconfigured\n");
    }

    u_puts("sockets: ");
    u_put_uint(info.used_sockets);
    u_putc('/');
    u_put_uint(info.max_sockets);
    u_puts(" used tcp=");
    u_put_uint(info.tcp_sockets);
    u_puts(" open=");
    u_put_uint(info.open_sockets);
    u_puts(" bound=");
    u_put_uint(info.bound_sockets);
    u_puts(" listen=");
    u_put_uint(info.listening_sockets);
    u_puts(" conn=");
    u_put_uint(info.connected_sockets);
    u_putc('\n');

    u_puts("tcp: listeners ");
    u_put_uint(info.tcp_listeners);
    u_putc('/');
    u_put_uint(info.tcp_max_listeners);
    u_puts(" conns ");
    u_put_uint(info.tcp_connections);
    u_putc('/');
    u_put_uint(info.tcp_max_connections);
    u_puts(" established=");
    u_put_uint(info.tcp_established_connections);
    u_puts(" accepted=");
    u_put_uint(info.tcp_accepted_connections);
    u_puts(" pending=");
    u_put_uint(info.tcp_pending_connections);
    u_puts(" syn=");
    u_put_uint(info.tcp_syn_recv_connections);
    u_puts(" fin=");
    u_put_uint(info.tcp_fin_wait_connections);
    u_putc('\n');

    u_puts("tcp buffers: rx ");
    u_put_uint(info.tcp_rx_rings);
    u_puts(" rings ");
    u_put_uint(info.tcp_rx_bytes);
    u_puts(" bytes queued / ");
    u_put_uint(info.tcp_rx_buffer_bytes / 1024u);
    u_puts(" KB allocated / ");
    u_put_uint(info.tcp_max_rx_buffer_bytes / 1024u);
    u_puts(" KB cap, tx ");
    u_put_uint(info.tcp_tx_rings);
    u_puts(" rings ");
    u_put_uint(info.tcp_tx_bytes);
    u_puts(" bytes queued / ");
    u_put_uint(info.tcp_tx_buffer_bytes / 1024u);
    u_puts(" KB allocated / ");
    u_put_uint(info.tcp_max_tx_buffer_bytes / 1024u);
    u_puts(" KB cap\n");

    sys_exit(0);
}
