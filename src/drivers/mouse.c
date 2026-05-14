#include "mouse.h"
#include "ports.h"
#include "input.h"

#define PS2_DATA        0x60
#define PS2_STATUS      0x64
#define PS2_COMMAND     0x64

#define PS2_STATUS_OUT  0x01
#define PS2_STATUS_IN   0x02
#define PS2_STATUS_AUX  0x20

#define PS2_CMD_READ_CONFIG  0x20
#define PS2_CMD_WRITE_CONFIG 0x60
#define PS2_CMD_ENABLE_AUX   0xA8
#define PS2_CMD_WRITE_AUX    0xD4

#define MOUSE_ACK 0xFA
#define MOUSE_CMD_SET_SAMPLE_RATE 0xF3
#define MOUSE_CMD_SET_RESOLUTION  0xE8
#define MOUSE_CMD_SET_SCALING_1_1 0xE6
#define MOUSE_CMD_SET_DEFAULTS    0xF6
#define MOUSE_CMD_ENABLE_STREAM   0xF4

#define MOUSE_SAMPLE_RATE 200u
#define MOUSE_RESOLUTION  3u

#define VMWARE_MAGIC 0x564D5868u
#define VMWARE_PORT  0x5658u

#define VMWARE_CMD_GET_VERSION        10u
#define VMWARE_CMD_ABSPOINTER_DATA    39u
#define VMWARE_CMD_ABSPOINTER_STATUS  40u
#define VMWARE_CMD_ABSPOINTER_COMMAND 41u

#define VMMOUSE_CMD_ENABLE           0x45414552u
#define VMMOUSE_CMD_REQUEST_ABSOLUTE 0x53424152u
#define VMMOUSE_ERROR                0xFFFF0000u
#define VMMOUSE_RELATIVE_PACKET      0x00010000u
#define VMMOUSE_LEFT_BUTTON          0x20u
#define VMMOUSE_RIGHT_BUTTON         0x10u
#define VMMOUSE_MIDDLE_BUTTON        0x08u
#define VMMOUSE_ABS_SCALE_SHIFT      6u

static int s_mouse_ready = 0;
static int s_vmmouse_ready = 0;
static unsigned char s_packet[3];
static unsigned int s_packet_pos = 0;
static int s_dx = 0;
static int s_dy = 0;
static unsigned int s_buttons = 0;
static unsigned int s_sequence = 0;
static unsigned int s_vmmouse_have_abs = 0;
static unsigned int s_vmmouse_last_x = 0;
static unsigned int s_vmmouse_last_y = 0;
static unsigned int s_irq_count = 0;
static unsigned int s_byte_count = 0;
static unsigned int s_aux_status_count = 0;
static unsigned int s_packet_count = 0;
static unsigned int s_vmmouse_packet_count = 0;
static unsigned int s_sync_drop_count = 0;
static unsigned int s_overflow_drop_count = 0;

static unsigned int irq_save(void) {
    unsigned int flags;

    __asm__ __volatile__(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory");
    return flags;
}

static void irq_restore(unsigned int flags) {
    __asm__ __volatile__(
        "pushl %0\n\t"
        "popfl"
        :
        : "r"(flags)
        : "memory", "cc");
}

static int ps2_wait_write(void) {
    for (unsigned int i = 0; i < 100000u; i++) {
        if ((inb(PS2_STATUS) & PS2_STATUS_IN) == 0) {
            return 1;
        }
    }
    return 0;
}

static int ps2_wait_read(void) {
    for (unsigned int i = 0; i < 100000u; i++) {
        if (inb(PS2_STATUS) & PS2_STATUS_OUT) {
            return 1;
        }
    }
    return 0;
}

static void ps2_flush_output(void) {
    for (unsigned int i = 0; i < 16u; i++) {
        if ((inb(PS2_STATUS) & PS2_STATUS_OUT) == 0) {
            break;
        }
        (void)inb(PS2_DATA);
    }
}

static int ps2_write_command(unsigned char command) {
    if (!ps2_wait_write()) {
        return 0;
    }
    outb(PS2_COMMAND, command);
    return 1;
}

static int ps2_write_data(unsigned char data) {
    if (!ps2_wait_write()) {
        return 0;
    }
    outb(PS2_DATA, data);
    return 1;
}

static int mouse_write(unsigned char command) {
    if (!ps2_write_command(PS2_CMD_WRITE_AUX)) {
        return 0;
    }
    if (!ps2_write_data(command)) {
        return 0;
    }
    if (!ps2_wait_read()) {
        return 0;
    }
    return inb(PS2_DATA) == MOUSE_ACK;
}

static int mouse_write_arg(unsigned char command, unsigned char value) {
    return mouse_write(command) && mouse_write(value);
}

static int ps2_read_config(unsigned char* config) {
    if (!ps2_write_command(PS2_CMD_READ_CONFIG)) {
        return 0;
    }
    if (!ps2_wait_read()) {
        return 0;
    }
    *config = inb(PS2_DATA);
    return 1;
}

static int ps2_write_config(unsigned char config) {
    if (!ps2_write_command(PS2_CMD_WRITE_CONFIG)) {
        return 0;
    }
    return ps2_write_data(config);
}

static void vmware_cmd(unsigned int in_bx,
                       unsigned int in_cx,
                       unsigned int* out_ax,
                       unsigned int* out_bx,
                       unsigned int* out_cx,
                       unsigned int* out_dx) {
    unsigned int ax = VMWARE_MAGIC;
    unsigned int bx = in_bx;
    unsigned int cx = in_cx;
    unsigned int dx = VMWARE_PORT;

    __asm__ __volatile__(
        "inl %%dx, %%eax"
        : "+a"(ax), "+b"(bx), "+c"(cx), "+d"(dx)
        :
        : "memory");

    if (out_ax) *out_ax = ax;
    if (out_bx) *out_bx = bx;
    if (out_cx) *out_cx = cx;
    if (out_dx) *out_dx = dx;
}

static int vmware_backdoor_available(void) {
    unsigned int ax;
    unsigned int bx;

    vmware_cmd(~VMWARE_MAGIC, VMWARE_CMD_GET_VERSION, &ax, &bx, 0, 0);
    return bx == VMWARE_MAGIC && ax != 0xFFFFFFFFu;
}

static void vmmouse_command(unsigned int command) {
    vmware_cmd(command, VMWARE_CMD_ABSPOINTER_COMMAND, 0, 0, 0, 0);
}

static unsigned int vmmouse_status(void) {
    unsigned int ax;

    vmware_cmd(0, VMWARE_CMD_ABSPOINTER_STATUS, &ax, 0, 0, 0);
    return ax;
}

static void vmmouse_data(unsigned int words,
                         unsigned int* status,
                         unsigned int* x,
                         unsigned int* y,
                         unsigned int* z) {
    vmware_cmd(words, VMWARE_CMD_ABSPOINTER_DATA, status, x, y, z);
}

static int vmmouse_init(void) {
    unsigned int status;
    unsigned int x;
    unsigned int y;
    unsigned int z;

    if (!vmware_backdoor_available()) {
        return 0;
    }

    vmmouse_command(VMMOUSE_CMD_ENABLE);
    (void)vmmouse_status();
    vmmouse_data(1u, &status, &x, &y, &z);
    (void)status;
    (void)x;
    (void)y;
    (void)z;
    vmmouse_command(VMMOUSE_CMD_REQUEST_ABSOLUTE);
    return 1;
}

static unsigned int vmmouse_buttons_to_ps2(unsigned int status) {
    unsigned int buttons = 0;

    if (status & VMMOUSE_LEFT_BUTTON) buttons |= SYS_MOUSE_BUTTON_LEFT;
    if (status & VMMOUSE_RIGHT_BUTTON) buttons |= SYS_MOUSE_BUTTON_RIGHT;
    if (status & VMMOUSE_MIDDLE_BUTTON) buttons |= SYS_MOUSE_BUTTON_MIDDLE;
    return buttons;
}

static int vmmouse_scaled_delta(unsigned int now, unsigned int last) {
    int delta = (int)now - (int)last;

    if (delta >= 0) {
        return delta >> VMMOUSE_ABS_SCALE_SHIFT;
    }
    return -(((-delta) >> VMMOUSE_ABS_SCALE_SHIFT));
}

static int vmmouse_drain_events(void) {
    unsigned int status;
    unsigned int queue_length;
    unsigned int x;
    unsigned int y;
    unsigned int z;
    unsigned int old_buttons;
    unsigned int new_buttons;
    unsigned int processed = 0;

    for (unsigned int i = 0; i < 255u; i++) {
        status = vmmouse_status();
        if ((status & VMMOUSE_ERROR) == VMMOUSE_ERROR) {
            s_vmmouse_ready = 0;
            return processed != 0u;
        }

        queue_length = status & 0xFFFFu;
        if (queue_length < 4u) {
            return processed != 0u;
        }

        vmmouse_data(4u, &status, &x, &y, &z);
        old_buttons = s_buttons;
        new_buttons = vmmouse_buttons_to_ps2(status);

        if (status & VMMOUSE_RELATIVE_PACKET) {
            int dx = (int)x;
            int dy = -((int)y);

            s_dx += dx;
            s_dy += dy;
            s_buttons = new_buttons;
            s_sequence++;
            s_packet_count++;
            s_vmmouse_packet_count++;
            input_push_mouse_event(dx, dy, s_buttons, old_buttons ^ s_buttons);
        } else {
            int dx = 0;
            int dy = 0;

            if (s_vmmouse_have_abs) {
                dx = vmmouse_scaled_delta(x, s_vmmouse_last_x);
                dy = vmmouse_scaled_delta(y, s_vmmouse_last_y);
            }
            s_vmmouse_last_x = x;
            s_vmmouse_last_y = y;
            s_vmmouse_have_abs = 1;

            s_dx += dx;
            s_dy += dy;
            s_buttons = new_buttons;
            s_sequence++;
            s_packet_count++;
            s_vmmouse_packet_count++;
            input_push_mouse_event(dx, dy, s_buttons, old_buttons ^ s_buttons);
        }
        processed++;
    }

    return processed != 0u;
}

int mouse_init(void) {
    unsigned char config;

    s_mouse_ready = 0;
    s_vmmouse_ready = 0;
    s_packet_pos = 0;
    s_dx = 0;
    s_dy = 0;
    s_buttons = 0;
    s_sequence = 0;
    s_vmmouse_have_abs = 0;
    s_vmmouse_last_x = 0;
    s_vmmouse_last_y = 0;
    s_irq_count = 0;
    s_byte_count = 0;
    s_aux_status_count = 0;
    s_packet_count = 0;
    s_vmmouse_packet_count = 0;
    s_sync_drop_count = 0;
    s_overflow_drop_count = 0;

    ps2_flush_output();
    if (!ps2_write_command(PS2_CMD_ENABLE_AUX)) {
        return 0;
    }
    if (!ps2_read_config(&config)) {
        return 0;
    }

    config |= 0x02u;   /* enable IRQ12 */
    config &= ~0x20u;  /* enable auxiliary clock */
    if (!ps2_write_config(config)) {
        return 0;
    }

    if (!mouse_write(MOUSE_CMD_SET_DEFAULTS) ||
        !mouse_write(MOUSE_CMD_SET_SCALING_1_1) ||
        !mouse_write_arg(MOUSE_CMD_SET_SAMPLE_RATE, MOUSE_SAMPLE_RATE) ||
        !mouse_write_arg(MOUSE_CMD_SET_RESOLUTION, MOUSE_RESOLUTION) ||
        !mouse_write(MOUSE_CMD_ENABLE_STREAM)) {
        return 0;
    }

    ps2_flush_output();
    s_mouse_ready = 1;
    s_vmmouse_ready = vmmouse_init();
    return 1;
}

int mouse_available(void) {
    return s_mouse_ready;
}

void mouse_handle_irq(void) {
    unsigned char status = inb(PS2_STATUS);
    unsigned char data;
    int dx;
    int dy;
    int event_dy;
    unsigned int old_buttons;

    s_irq_count++;
    if ((status & PS2_STATUS_OUT) == 0) {
        return;
    }

    data = inb(PS2_DATA);
    s_byte_count++;
    if (status & PS2_STATUS_AUX) {
        s_aux_status_count++;
    }
    if (!s_mouse_ready) {
        return;
    }

    if (s_vmmouse_ready && vmmouse_drain_events()) {
        return;
    }

    if (s_packet_pos == 0 && (data & 0x08u) == 0) {
        s_sync_drop_count++;
        return;
    }

    s_packet[s_packet_pos++] = data;
    if (s_packet_pos < 3u) {
        return;
    }
    s_packet_pos = 0;

    if (s_packet[0] & 0xC0u) {
        s_overflow_drop_count++;
        return;
    }

    dx = (int)s_packet[1];
    dy = (int)s_packet[2];
    if (s_packet[0] & 0x10u) dx -= 256;
    if (s_packet[0] & 0x20u) dy -= 256;

    event_dy = -dy;
    old_buttons = s_buttons;

    s_dx += dx;
    s_dy += event_dy;
    s_buttons = s_packet[0] & 0x07u;
    s_sequence++;
    s_packet_count++;
    input_push_mouse_event(dx, event_dy, s_buttons, old_buttons ^ s_buttons);
}

int mouse_read_state(sys_mouse_state_t* out) {
    unsigned int flags;

    if (!s_mouse_ready || !out) {
        return 0;
    }

    flags = irq_save();
    out->dx = s_dx;
    out->dy = s_dy;
    out->buttons = s_buttons;
    out->sequence = s_sequence;
    s_dx = 0;
    s_dy = 0;
    irq_restore(flags);
    return 1;
}

void mouse_debug_snapshot(mouse_debug_state_t* out) {
    unsigned int flags;

    if (!out) {
        return;
    }

    flags = irq_save();
    out->irq_count = s_irq_count;
    out->byte_count = s_byte_count;
    out->aux_status_count = s_aux_status_count;
    out->packet_count = s_packet_count;
    out->vmware_packet_count = s_vmmouse_packet_count;
    out->sync_drop_count = s_sync_drop_count;
    out->overflow_drop_count = s_overflow_drop_count;
    out->vmware_enabled = (unsigned int)s_vmmouse_ready;
    irq_restore(flags);
}
