#include "diag_util.h"

static void put_int(int value) {
    if (value < 0) {
        u_putc('-');
        u_put_uint((unsigned int)(-value));
    } else {
        u_put_uint((unsigned int)value);
    }
}

static void print_unavailable(const sys_mousedebug_t* debug) {
    u_puts("mousetest: mouse unavailable\n");
    u_puts("mousetest: ready=");
    u_put_uint(debug->ready);
    u_puts(" init_step=");
    u_put_uint(debug->init_step);
    u_puts(" init_fail=");
    u_put_uint(debug->init_fail);
    u_puts(" cfg=");
    diag_put_hex32(debug->config_before);
    u_putc('/');
    diag_put_hex32(debug->config_after);
    u_puts(" irq=");
    u_put_uint(debug->irq_count);
    u_puts(" bytes=");
    u_put_uint(debug->byte_count);
    u_puts(" aux=");
    u_put_uint(debug->aux_status_count);
    u_puts(" packets=");
    u_put_uint(debug->packet_count);
    u_puts(" packet_size=");
    u_put_uint(debug->packet_size);
    u_puts(" device_id=");
    u_put_uint(debug->device_id);
    u_putc('\n');
}

void _start(int argc, char** argv) {
    sys_mouse_state_t mouse;
    sys_mousedebug_t before;
    sys_mousedebug_t after;
    unsigned int deadline;
    unsigned int last_sequence;
    unsigned int events = 0u;
    int usb_open;

    (void)argc;
    (void)argv;

    usb_open = sys_usb_mouse_op(SYS_USB_MOUSE_OP_OPEN, 0);
    if (usb_open > 0) {
        u_puts("mousetest: usb poll active\n");
    }

    if (sys_mouse_read(&mouse) < 0) {
        if (sys_mouse_debug(&after) == 0) {
            print_unavailable(&after);
        } else {
            u_puts("mousetest: mouse unavailable\n");
        }
        if (usb_open > 0) {
            (void)sys_usb_mouse_op(SYS_USB_MOUSE_OP_CLOSE, 0);
        }
        sys_exit(1);
    }

    last_sequence = mouse.sequence;
    if (sys_mouse_debug(&before) < 0) {
        u_puts("mousetest: debug counters unavailable\n");
        before = (sys_mousedebug_t){0};
    }

    deadline = sys_get_ticks() + 500u;
    u_puts("mousetest: move/click mouse for 5 seconds\n");
    while ((int)(sys_get_ticks() - deadline) < 0) {
        if (usb_open > 0) {
            int usb_poll = sys_usb_mouse_op(SYS_USB_MOUSE_OP_POLL, 0);
            if (usb_poll < 0) usb_open = 0;
        }
        if (sys_mouse_read(&mouse) < 0) {
            u_puts("mousetest: mouse became unavailable\n");
            if (usb_open > 0) {
                (void)sys_usb_mouse_op(SYS_USB_MOUSE_OP_CLOSE, 0);
            }
            sys_exit(1);
        }
        if (mouse.sequence != last_sequence ||
            mouse.dx != 0 || mouse.dy != 0 || mouse.wheel != 0) {
            last_sequence = mouse.sequence;
            events++;
            u_puts("mousetest: seq=");
            u_put_uint(mouse.sequence);
            u_puts(" dx=");
            put_int(mouse.dx);
            u_puts(" dy=");
            put_int(mouse.dy);
            u_puts(" wheel=");
            put_int(mouse.wheel);
            u_puts(" buttons=");
            u_put_uint(mouse.buttons);
            u_putc('\n');
        }
        sys_yield();
    }

    u_puts("mousetest: events=");
    u_put_uint(events);
    u_putc('\n');

    if (sys_mouse_debug(&after) == 0) {
        u_puts("mousetest: irq=");
        u_put_uint(after.irq_count - before.irq_count);
        u_puts(" bytes=");
        u_put_uint(after.byte_count - before.byte_count);
        u_puts(" aux=");
        u_put_uint(after.aux_status_count - before.aux_status_count);
        u_puts(" packets=");
        u_put_uint(after.packet_count - before.packet_count);
        u_puts(" vmware=");
        u_put_uint(after.vmware_packet_count - before.vmware_packet_count);
        u_puts(" vmware_on=");
        u_put_uint(after.vmware_enabled);
        u_puts(" packet_size=");
        u_put_uint(after.packet_size);
        u_puts(" device_id=");
        u_put_uint(after.device_id);
        u_puts(" ready=");
        u_put_uint(after.ready);
        u_puts(" init=");
        u_put_uint(after.init_step);
        u_putc('/');
        u_put_uint(after.init_fail);
        u_puts(" syncdrop=");
        u_put_uint(after.sync_drop_count - before.sync_drop_count);
        u_puts(" overflow=");
        u_put_uint(after.overflow_drop_count - before.overflow_drop_count);
        u_putc('\n');
    }

    if (usb_open > 0) {
        (void)sys_usb_mouse_op(SYS_USB_MOUSE_OP_CLOSE, 0);
    }
    sys_exit(0);
}
