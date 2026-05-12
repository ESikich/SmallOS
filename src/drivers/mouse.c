#include "mouse.h"
#include "ports.h"

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

static int s_mouse_ready = 0;
static unsigned char s_packet[3];
static unsigned int s_packet_pos = 0;
static int s_dx = 0;
static int s_dy = 0;
static unsigned int s_buttons = 0;
static unsigned int s_sequence = 0;

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

int mouse_init(void) {
    unsigned char config;

    s_mouse_ready = 0;
    s_packet_pos = 0;
    s_dx = 0;
    s_dy = 0;
    s_buttons = 0;
    s_sequence = 0;

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

    if ((status & PS2_STATUS_OUT) == 0) {
        return;
    }

    data = inb(PS2_DATA);
    if (!s_mouse_ready || (status & PS2_STATUS_AUX) == 0) {
        return;
    }

    if (s_packet_pos == 0 && (data & 0x08u) == 0) {
        return;
    }

    s_packet[s_packet_pos++] = data;
    if (s_packet_pos < 3u) {
        return;
    }
    s_packet_pos = 0;

    if (s_packet[0] & 0xC0u) {
        return;
    }

    dx = (int)s_packet[1];
    dy = (int)s_packet[2];
    if (s_packet[0] & 0x10u) dx -= 256;
    if (s_packet[0] & 0x20u) dy -= 256;

    s_dx += dx;
    s_dy -= dy;
    s_buttons = s_packet[0] & 0x07u;
    s_sequence++;
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
