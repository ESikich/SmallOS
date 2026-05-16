#include "diag_util.h"

void _start(int argc, char** argv) {
    sys_usbinfo_t usb;

    (void)argc;
    (void)argv;

    if (sys_usbinfo(&usb) < 0) {
        u_puts("usbinfo: unavailable\n");
        sys_exit(1);
    }

    u_puts("usbinfo: controllers=");
    u_put_uint(usb.controller_count);
    u_puts(" uhci=");
    u_put_uint(usb.uhci_count);
    u_puts(" ohci=");
    u_put_uint(usb.ohci_count);
    u_puts(" ehci=");
    u_put_uint(usb.ehci_count);
    u_puts(" xhci=");
    u_put_uint(usb.xhci_count);
    u_puts(" powered_ports=");
    u_put_uint(usb.powered_port_count);
    u_putc('\n');

    u_puts("usbinfo: hid keyboard=");
    u_put_uint(usb.keyboard_active);
    u_puts(" port=");
    u_put_uint(usb.keyboard_port);
    u_puts(" ep=");
    u_put_uint(usb.keyboard_endpoint);
    u_puts(" pkt=");
    u_put_uint(usb.keyboard_packet_size);
    u_puts(" int=");
    u_put_uint(usb.keyboard_interval);
    u_puts(" polls=");
    u_put_uint(usb.keyboard_poll_count);
    u_puts(" reports=");
    u_put_uint(usb.keyboard_report_count);
    u_puts(" fail=");
    u_put_uint(usb.keyboard_fail_count);
    u_puts(" cc=");
    diag_put_hex32(usb.keyboard_last_cc);
    u_putc('\n');

    u_puts("usbinfo: hid mouse=");
    u_put_uint(usb.mouse_active);
    u_puts(" port=");
    u_put_uint(usb.mouse_port);
    u_puts(" ep=");
    u_put_uint(usb.mouse_endpoint);
    u_puts(" pkt=");
    u_put_uint(usb.mouse_packet_size);
    u_puts(" int=");
    u_put_uint(usb.mouse_interval);
    u_puts(" polls=");
    u_put_uint(usb.mouse_poll_count);
    u_puts(" reports=");
    u_put_uint(usb.mouse_report_count);
    u_puts(" fail=");
    u_put_uint(usb.mouse_fail_count);
    u_puts(" cc=");
    diag_put_hex32(usb.mouse_last_cc);
    u_putc('\n');

    u_puts("usbinfo: hid service=");
    u_put_uint(usb.service_active);
    u_puts(" storage=");
    u_put_uint(usb.storage_active);
    u_puts(" port=");
    u_put_uint(usb.storage_port);
    u_putc('\n');

    u_puts("usbinfo: last=");
    diag_put_hex32(((unsigned int)usb.last_bus << 8) |
                   ((unsigned int)usb.last_slot << 3) |
                   (unsigned int)usb.last_func);
    u_puts(" prog=");
    diag_put_hex32(usb.last_prog_if);
    u_puts(" bar=");
    diag_put_hex32(usb.last_bar);
    u_puts(" ports=");
    u_put_uint(usb.last_ports);
    u_puts(" status=");
    diag_put_hex32(usb.last_port_status0);
    u_putc('/');
    diag_put_hex32(usb.last_port_status1);
    u_putc('\n');

    sys_exit(0);
}
