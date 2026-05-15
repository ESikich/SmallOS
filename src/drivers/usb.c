#include "usb.h"

#include "keyboard.h"
#include "mouse.h"
#include "pci.h"
#include "terminal.h"
#include "../kernel/klib.h"
#include "../kernel/paging.h"
#include "../kernel/ports.h"
#include "../kernel/process.h"
#include "../kernel/scheduler.h"
#include "../kernel/timer.h"
#include "../kernel/types.h"

#define PCI_CLASS_SERIAL_BUS 0x0Cu
#define PCI_SUBCLASS_USB     0x03u

#define USB_PROG_UHCI 0x00u
#define USB_PROG_OHCI 0x10u
#define USB_PROG_EHCI 0x20u
#define USB_PROG_XHCI 0x30u

#define UHCI_BAR_IO_BASE 0x20u

#define PCI_COMMAND_MEMORY 0x0002u

#define OHCI_HC_CONTROL       0x04u
#define OHCI_HC_COMMAND_STATUS 0x08u
#define OHCI_HC_INTERRUPT_STATUS 0x0Cu
#define OHCI_HC_INTERRUPT_ENABLE 0x10u
#define OHCI_HC_INTERRUPT_DISABLE 0x14u
#define OHCI_HC_CONTROL_HEAD  0x18u
#define OHCI_HC_CONTROL_CUR   0x1Cu
#define OHCI_HC_HCCA          0x18u
#define OHCI_HC_PERIOD_CUR    0x1Cu
#define OHCI_HC_CONTROL_HEAD_REG 0x20u
#define OHCI_HC_CONTROL_CUR_REG  0x24u
#define OHCI_HC_BULK_HEAD     0x28u
#define OHCI_HC_DONE_HEAD     0x30u
#define OHCI_HC_FM_INTERVAL   0x34u
#define OHCI_HC_PERIOD_START  0x40u
#define OHCI_HC_LS_THRESHOLD  0x44u
#define OHCI_HC_RH_DESC_A     0x48u
#define OHCI_HC_RH_STATUS     0x50u
#define OHCI_HC_RH_PORT_BASE  0x54u
#define OHCI_CONTROL_PLE      0x00000004u
#define OHCI_CONTROL_CLE      0x00000010u
#define OHCI_CONTROL_USB_OPERATIONAL 0x00000080u
#define OHCI_CONTROL_INTERRUPT_ROUTING 0x00000100u
#define OHCI_CMD_CLF          0x00000002u
#define OHCI_INT_ALL          0xC000007Fu
#define OHCI_RHDA_NPS         0x00000200u
#define OHCI_RHDA_PSM         0x00000100u
#define OHCI_RHS_LPSC         0x00010000u
#define OHCI_PORT_CCS         0x00000001u
#define OHCI_PORT_PES         0x00000002u
#define OHCI_PORT_PSS         0x00000004u
#define OHCI_PORT_PRS         0x00000010u
#define OHCI_PORT_PPS         0x00000100u
#define OHCI_PORT_LSDA        0x00000200u
#define OHCI_PORT_PRSC        0x00100000u

#define OHCI_ED_CTRL_MPS_SHIFT 16u
#define OHCI_ED_CTRL_SPEED_LOW 0x00002000u
#define OHCI_ED_CTRL_SKIP      0x00004000u
#define OHCI_ED_CTRL_DIR_TD    0x00000000u
#define OHCI_ED_CTRL_DIR_OUT   0x00000800u
#define OHCI_ED_CTRL_DIR_IN    0x00001000u
#define OHCI_ED_HEAD_HALTED    0x00000001u
#define OHCI_ED_HEAD_TOGGLE    0x00000002u

#define OHCI_TD_CC_SHIFT       28u
#define OHCI_TD_CC_NOT_ACCESSED 0xFu
#define OHCI_TD_DP_SETUP       0x00000000u
#define OHCI_TD_DP_OUT         0x00080000u
#define OHCI_TD_DP_IN          0x00100000u
#define OHCI_TD_BUFFER_ROUNDING 0x00040000u
#define OHCI_TD_DI_NONE        0x00E00000u
#define OHCI_TD_TOGGLE_CARRY   0x00000000u
#define OHCI_TD_TOGGLE_DATA0   0x02000000u
#define OHCI_TD_TOGGLE_DATA1   0x03000000u

#define USB_REQ_GET_DESCRIPTOR 0x06u
#define USB_REQ_SET_ADDRESS    0x05u
#define USB_REQ_SET_CONFIGURATION 0x09u
#define USB_REQ_SET_IDLE       0x0Au
#define USB_REQ_SET_PROTOCOL   0x0Bu

#define USB_DESC_DEVICE        0x01u
#define USB_DESC_CONFIGURATION 0x02u

#define USB_CLASS_HID          0x03u
#define USB_HID_SUBCLASS_BOOT  0x01u
#define USB_HID_PROTOCOL_KEYBOARD 0x01u
#define USB_HID_PROTOCOL_MOUSE 0x02u

#define USB_ENDPOINT_DIR_IN    0x80u
#define USB_ENDPOINT_TYPE_INTERRUPT 0x03u

#define EHCI_CAP_HCS_PARAMS   0x04u
#define EHCI_OP_PORT_BASE     0x44u

#define USB_MAX_CONTROLLERS   8u

typedef struct {
    volatile u32 control;
    volatile u32 tail_td;
    volatile u32 head_td;
    volatile u32 next_ed;
} __attribute__((packed, aligned(16))) ohci_ed_t;

typedef struct {
    volatile u32 control;
    volatile u32 cbp;
    volatile u32 next_td;
    volatile u32 be;
} __attribute__((packed, aligned(16))) ohci_td_t;

typedef struct {
    volatile u32 interrupt_table[32];
    volatile unsigned short frame_number;
    volatile unsigned short pad1;
    volatile u32 done_head;
    u8 reserved[116];
} __attribute__((packed, aligned(256))) ohci_hcca_t;

typedef struct {
    u8 bm_request_type;
    u8 b_request;
    unsigned short w_value;
    unsigned short w_index;
    unsigned short w_length;
} __attribute__((packed)) usb_setup_packet_t;

typedef struct {
    u32 control;
    u32 interrupt_enable;
    u32 hcca;
    u32 control_head;
    u32 control_cur;
    u32 bulk_head;
} ohci_saved_state_t;

typedef struct {
    volatile u32* regs;
    unsigned int bar;
    unsigned int port_index;
    unsigned int low_speed;
    unsigned int address;
    unsigned int endpoint;
    unsigned int interface_number;
    unsigned int packet_size;
    unsigned int interval;
    unsigned int protocol;
    int has_saved_state;
    ohci_saved_state_t saved_state;
} usb_mouse_dev_t;

typedef struct {
    int present;
    pci_device_t pci;
    unsigned int bar;
    volatile u32* regs;
    unsigned int ports;
} usb_controller_t;

static ohci_hcca_t s_ohci_hcca __attribute__((aligned(256)));
static ohci_ed_t s_control_ed __attribute__((aligned(16)));
static ohci_ed_t s_intr_ed __attribute__((aligned(16)));
static ohci_ed_t s_keyboard_intr_ed __attribute__((aligned(16)));
static ohci_td_t s_tds[8] __attribute__((aligned(16)));
static ohci_td_t s_intr_td[2] __attribute__((aligned(16)));
static ohci_td_t s_keyboard_intr_td[2] __attribute__((aligned(16)));
static usb_setup_packet_t s_setup __attribute__((aligned(16)));
static u8 s_usb_buf[256] __attribute__((aligned(16)));
static u8 s_report_buf[8] __attribute__((aligned(16)));
static u8 s_keyboard_report_buf[8] __attribute__((aligned(16)));
static u8 s_keyboard_last_report[8] __attribute__((aligned(16)));

static usb_debug_state_t s_usb_debug;
static usb_controller_t s_usb_controllers[USB_MAX_CONTROLLERS];
static unsigned int s_usb_controller_count = 0;
static int s_ohci_dry_run = 0;
static int s_ohci_verbose = 0;
static int s_usb_mouse_log = 1;
static usb_mouse_dev_t s_usb_mouse_active_dev;
static usb_mouse_dev_t s_usb_keyboard_active_dev;
static int s_usb_mouse_active = 0;
static int s_usb_keyboard_active = 0;
static int s_usb_service_active = 0;
static int s_usb_service_started = 0;
static int s_usb_service_probe_done = 0;
static int s_usb_mouse_active_log = 1;
static unsigned int s_usb_mouse_last_buttons = 0;

static void usb_delay(void) {
    for (volatile unsigned int i = 0; i < 100000u; i++) {
        __asm__ __volatile__("" : : : "memory");
    }
}

static void usb_put_byte_hex(unsigned char value) {
    static const char hex[] = "0123456789ABCDEF";
    terminal_putc(hex[(value >> 4) & 0xFu]);
    terminal_putc(hex[value & 0xFu]);
}

static void usb_print_addr(const pci_device_t* dev) {
    usb_put_byte_hex(dev->bus);
    terminal_putc(':');
    usb_put_byte_hex(dev->slot);
    terminal_putc('.');
    terminal_putc((char)('0' + dev->func));
}

static unsigned int usb_controller_bar(const pci_device_t* dev) {
    unsigned int bar;

    if (dev->prog_if == USB_PROG_UHCI) {
        bar = pci_read_config_dword(dev->bus, dev->slot, dev->func, UHCI_BAR_IO_BASE);
        return bar & 0xFFFFFFFCu;
    }

    bar = pci_read_config_dword(dev->bus, dev->slot, dev->func, 0x10);
    return bar & 0xFFFFFFF0u;
}

static void usb_enable_mmio(const pci_device_t* dev) {
    unsigned short cmd = pci_read_config_word(dev->bus, dev->slot, dev->func, 0x04);

    if ((cmd & PCI_COMMAND_MEMORY) == 0u) {
        pci_write_config_word(dev->bus,
                              dev->slot,
                              dev->func,
                              0x04,
                              (unsigned short)(cmd | PCI_COMMAND_MEMORY));
    }
}

static void usb_map_mmio(u32 phys, u32 size) {
    u32* pd = paging_get_kernel_pd();

    for (u32 off = 0; off < size; off += PAGE_SIZE) {
        paging_map_page(pd, phys + off, phys + off, PAGE_WRITE);
    }
}

static void usb_register_controller(const pci_device_t* dev) {
    usb_controller_t* ctrl;
    unsigned int bar = usb_controller_bar(dev);

    if (s_usb_controller_count >= USB_MAX_CONTROLLERS) {
        return;
    }

    ctrl = &s_usb_controllers[s_usb_controller_count++];
    k_memset(ctrl, 0, sizeof(*ctrl));
    ctrl->present = 1;
    ctrl->pci = *dev;
    ctrl->bar = bar;

    if (bar != 0u && dev->prog_if != USB_PROG_UHCI) {
        usb_enable_mmio(dev);
        usb_map_mmio(bar, PAGE_SIZE);
        ctrl->regs = (volatile u32*)bar;

        if (dev->prog_if == USB_PROG_OHCI) {
            ctrl->ports = ctrl->regs[OHCI_HC_RH_DESC_A / 4u] & 0xFFu;
            if (ctrl->ports > 15u) ctrl->ports = 15u;
        } else if (dev->prog_if == USB_PROG_EHCI) {
            volatile u8* base8 = (volatile u8*)bar;
            volatile u32* cap = (volatile u32*)bar;
            u8 cap_len = base8[0];
            u32 hcs = cap[EHCI_CAP_HCS_PARAMS / 4u];
            (void)cap_len;
            ctrl->ports = hcs & 0x0Fu;
            if (ctrl->ports > 15u) ctrl->ports = 15u;
        }
    }
}

static void ohci_bind_hcca(volatile u32* regs) {
    k_memset(&s_ohci_hcca, 0, sizeof(s_ohci_hcca));
    regs[OHCI_HC_HCCA / 4u] = (u32)(unsigned int)&s_ohci_hcca;
    if (s_ohci_verbose) {
        terminal_puts("usbmouse: hcca=");
        terminal_put_hex((u32)(unsigned int)&s_ohci_hcca);
        terminal_putc('\n');
    }
}

static void ohci_quiet_interrupts(volatile u32* regs) {
    regs[OHCI_HC_INTERRUPT_DISABLE / 4u] = OHCI_INT_ALL;
    regs[OHCI_HC_INTERRUPT_STATUS / 4u] = regs[OHCI_HC_INTERRUPT_STATUS / 4u];
}

static void ohci_save_state(volatile u32* regs, ohci_saved_state_t* state) {
    state->control = regs[OHCI_HC_CONTROL / 4u];
    state->interrupt_enable = regs[OHCI_HC_INTERRUPT_ENABLE / 4u];
    state->hcca = regs[OHCI_HC_HCCA / 4u];
    state->control_head = regs[OHCI_HC_CONTROL_HEAD_REG / 4u];
    state->control_cur = regs[OHCI_HC_CONTROL_CUR_REG / 4u];
    state->bulk_head = regs[OHCI_HC_BULK_HEAD / 4u];
}

static void ohci_restore_state(volatile u32* regs, const ohci_saved_state_t* state) {
    if (s_usb_mouse_log) {
        terminal_puts("usbmouse: restore ohci\n");
    }
    regs[OHCI_HC_INTERRUPT_DISABLE / 4u] = OHCI_INT_ALL;
    regs[OHCI_HC_CONTROL / 4u] =
        regs[OHCI_HC_CONTROL / 4u] & ~(OHCI_CONTROL_CLE | OHCI_CONTROL_PLE);
    regs[OHCI_HC_CONTROL_HEAD_REG / 4u] = state->control_head;
    regs[OHCI_HC_CONTROL_CUR_REG / 4u] = state->control_cur;
    regs[OHCI_HC_BULK_HEAD / 4u] = state->bulk_head;
    regs[OHCI_HC_HCCA / 4u] = state->hcca;
    regs[OHCI_HC_INTERRUPT_STATUS / 4u] = regs[OHCI_HC_INTERRUPT_STATUS / 4u];
    regs[OHCI_HC_INTERRUPT_ENABLE / 4u] = state->interrupt_enable;
    regs[OHCI_HC_CONTROL / 4u] = state->control;
}

static void ohci_print_state(volatile u32* regs, const char* tag) {
    terminal_puts("usbmouse: ");
    terminal_puts(tag);
    terminal_puts(" ctrl=");
    terminal_put_hex(regs[OHCI_HC_CONTROL / 4u]);
    terminal_puts(" cmd=");
    terminal_put_hex(regs[OHCI_HC_COMMAND_STATUS / 4u]);
    terminal_puts(" ints=");
    terminal_put_hex(regs[OHCI_HC_INTERRUPT_STATUS / 4u]);
    terminal_puts(" hcca=");
    terminal_put_hex(regs[OHCI_HC_HCCA / 4u]);
    terminal_putc('\n');
}

static void usb_note_controller(const pci_device_t* dev) {
    s_usb_debug.controller_count++;
    s_usb_debug.last_bus = dev->bus;
    s_usb_debug.last_slot = dev->slot;
    s_usb_debug.last_func = dev->func;
    s_usb_debug.last_prog_if = dev->prog_if;
    s_usb_debug.last_bar = usb_controller_bar(dev);

    if (dev->prog_if == USB_PROG_UHCI) {
        s_usb_debug.uhci_count++;
    } else if (dev->prog_if == USB_PROG_OHCI) {
        s_usb_debug.ohci_count++;
    } else if (dev->prog_if == USB_PROG_EHCI) {
        s_usb_debug.ehci_count++;
    } else if (dev->prog_if == USB_PROG_XHCI) {
        s_usb_debug.xhci_count++;
    }
}

static void usb_log_controller(const pci_device_t* dev) {
    terminal_puts("usb: controller ");
    usb_print_addr(dev);
    terminal_puts(" vendor=");
    terminal_put_hex(dev->vendor_id);
    terminal_puts(" device=");
    terminal_put_hex(dev->device_id);
    terminal_puts(" prog=");
    usb_put_byte_hex(dev->prog_if);
    terminal_puts(" bar=");
    terminal_put_hex(usb_controller_bar(dev));
    terminal_putc('\n');
}

void usb_init(void) {
    s_usb_debug = (usb_debug_state_t){0};
    k_memset(s_usb_controllers, 0, sizeof(s_usb_controllers));
    s_usb_controller_count = 0;
    terminal_puts("usb: passive probe\n");

    for (unsigned int bus = 0; bus < 256u; bus++) {
        for (unsigned int slot = 0; slot < 32u; slot++) {
            unsigned short vendor = pci_read_config_word((unsigned char)bus,
                                                         (unsigned char)slot,
                                                         0,
                                                         0x00);
            if (vendor == 0xFFFFu) {
                continue;
            }

            unsigned char header_type = pci_read_config_byte((unsigned char)bus,
                                                             (unsigned char)slot,
                                                             0,
                                                             0x0E);
            unsigned int function_count = (header_type & 0x80u) ? 8u : 1u;

            for (unsigned int func = 0; func < function_count; func++) {
                pci_device_t dev;

                pci_read_device((unsigned char)bus,
                                (unsigned char)slot,
                                (unsigned char)func,
                                &dev);
                if (dev.vendor_id == 0xFFFFu ||
                    dev.class_code != PCI_CLASS_SERIAL_BUS ||
                    dev.subclass != PCI_SUBCLASS_USB) {
                    continue;
                }

                usb_note_controller(&dev);
                usb_register_controller(&dev);
                usb_log_controller(&dev);
            }
        }
    }

    terminal_puts("usb: controllers=");
    terminal_put_uint(s_usb_debug.controller_count);
    terminal_puts(" uhci=");
    terminal_put_uint(s_usb_debug.uhci_count);
    terminal_puts(" ohci=");
    terminal_put_uint(s_usb_debug.ohci_count);
    terminal_puts(" ehci=");
    terminal_put_uint(s_usb_debug.ehci_count);
    terminal_puts(" xhci=");
    terminal_put_uint(s_usb_debug.xhci_count);
    terminal_puts(" mapped=");
    terminal_put_uint(s_usb_controller_count);
    terminal_putc('\n');
}

static void usb_dump_port_flags(u32 status, int ohci) {
    terminal_puts(" flags=");
    if (status & 0x00000001u) terminal_puts("conn,");
    if (status & 0x00000002u) terminal_puts("en,");
    if (status & 0x00000004u) terminal_puts(ohci ? "suspend," : "change,");
    if (status & 0x00000010u) terminal_puts("reset,");
    if (ohci && (status & OHCI_PORT_PPS)) terminal_puts("power,");
    if (ohci && (status & OHCI_PORT_LSDA)) terminal_puts("low,");
    if (!ohci && (status & 0x00001000u)) terminal_puts("power,");
}

void usb_dump_ports(void) {
    terminal_puts("usbports: passive port dump\n");

    if (s_usb_controller_count != 0u) {
        for (unsigned int i = 0; i < s_usb_controller_count; i++) {
            usb_controller_t* ctrl = &s_usb_controllers[i];
            pci_device_t* dev = &ctrl->pci;

            terminal_puts("usbports: ctrl ");
            usb_print_addr(dev);
            terminal_puts(" prog=");
            terminal_put_hex(dev->prog_if);
            terminal_puts(" bar=");
            terminal_put_hex(ctrl->bar);
            terminal_putc('\n');

            if (!ctrl->regs) {
                continue;
            }

            if (dev->prog_if == USB_PROG_OHCI) {
                u32 desc_a = ctrl->regs[OHCI_HC_RH_DESC_A / 4u];
                u32 ports = ctrl->ports;
                terminal_puts("usbports: ohci ports=");
                terminal_put_uint(ports);
                terminal_puts(" rhda=");
                terminal_put_hex(desc_a);
                terminal_putc('\n');
                for (u32 port = 0; port < ports; port++) {
                    u32 st = ctrl->regs[(OHCI_HC_RH_PORT_BASE + port * 4u) / 4u];
                    terminal_puts("usbports: ohci port=");
                    terminal_put_uint(port + 1u);
                    terminal_puts(" st=");
                    terminal_put_hex(st);
                    usb_dump_port_flags(st, 1);
                    terminal_putc('\n');
                }
            } else if (dev->prog_if == USB_PROG_EHCI) {
                volatile u8* base8 = (volatile u8*)ctrl->bar;
                u8 cap_len = base8[0];
                volatile u32* cap = (volatile u32*)ctrl->bar;
                volatile u32* op = (volatile u32*)(ctrl->bar + cap_len);
                u32 hcs = cap[EHCI_CAP_HCS_PARAMS / 4u];
                u32 ports = ctrl->ports;
                terminal_puts("usbports: ehci ports=");
                terminal_put_uint(ports);
                terminal_puts(" caplen=");
                terminal_put_uint(cap_len);
                terminal_puts(" hcs=");
                terminal_put_hex(hcs);
                terminal_putc('\n');
                for (u32 port = 0; port < ports; port++) {
                    u32 st = op[(EHCI_OP_PORT_BASE + port * 4u) / 4u];
                    terminal_puts("usbports: ehci port=");
                    terminal_put_uint(port + 1u);
                    terminal_puts(" st=");
                    terminal_put_hex(st);
                    usb_dump_port_flags(st, 0);
                    terminal_putc('\n');
                }
            }
        }
        return;
    }

    for (unsigned int bus = 0; bus < 256u; bus++) {
        for (unsigned int slot = 0; slot < 32u; slot++) {
            unsigned short vendor = pci_read_config_word((unsigned char)bus,
                                                         (unsigned char)slot,
                                                         0,
                                                         0x00);
            if (vendor == 0xFFFFu) {
                continue;
            }

            unsigned char header_type = pci_read_config_byte((unsigned char)bus,
                                                             (unsigned char)slot,
                                                             0,
                                                             0x0E);
            unsigned int function_count = (header_type & 0x80u) ? 8u : 1u;

            for (unsigned int func = 0; func < function_count; func++) {
                pci_device_t dev;
                unsigned int bar;
                volatile u32* regs;

                pci_read_device((unsigned char)bus,
                                (unsigned char)slot,
                                (unsigned char)func,
                                &dev);
                if (dev.vendor_id == 0xFFFFu ||
                    dev.class_code != PCI_CLASS_SERIAL_BUS ||
                    dev.subclass != PCI_SUBCLASS_USB) {
                    continue;
                }

                bar = usb_controller_bar(&dev);
                terminal_puts("usbports: ctrl ");
                usb_print_addr(&dev);
                terminal_puts(" prog=");
                terminal_put_hex(dev.prog_if);
                terminal_puts(" bar=");
                terminal_put_hex(bar);
                terminal_putc('\n');

                if (bar == 0 || dev.prog_if == USB_PROG_UHCI ||
                    dev.prog_if == USB_PROG_XHCI) {
                    continue;
                }

                usb_map_mmio(bar, PAGE_SIZE);
                regs = (volatile u32*)bar;

                if (dev.prog_if == USB_PROG_OHCI) {
                    u32 desc_a = regs[OHCI_HC_RH_DESC_A / 4u];
                    u32 ports = desc_a & 0xFFu;
                    if (ports > 15u) ports = 15u;
                    terminal_puts("usbports: ohci ports=");
                    terminal_put_uint(ports);
                    terminal_puts(" rhda=");
                    terminal_put_hex(desc_a);
                    terminal_putc('\n');
                    for (u32 port = 0; port < ports; port++) {
                        u32 st = regs[(OHCI_HC_RH_PORT_BASE + port * 4u) / 4u];
                        terminal_puts("usbports: ohci port=");
                        terminal_put_uint(port + 1u);
                        terminal_puts(" st=");
                        terminal_put_hex(st);
                        usb_dump_port_flags(st, 1);
                        terminal_putc('\n');
                    }
                } else if (dev.prog_if == USB_PROG_EHCI) {
                    volatile u8* base8 = (volatile u8*)bar;
                    u8 cap_len = base8[0];
                    volatile u32* cap = (volatile u32*)bar;
                    volatile u32* op = (volatile u32*)(bar + cap_len);
                    u32 hcs = cap[EHCI_CAP_HCS_PARAMS / 4u];
                    u32 ports = hcs & 0x0Fu;
                    if (ports > 15u) ports = 15u;
                    terminal_puts("usbports: ehci ports=");
                    terminal_put_uint(ports);
                    terminal_puts(" caplen=");
                    terminal_put_uint(cap_len);
                    terminal_puts(" hcs=");
                    terminal_put_hex(hcs);
                    terminal_putc('\n');
                    for (u32 port = 0; port < ports; port++) {
                        u32 st = op[(EHCI_OP_PORT_BASE + port * 4u) / 4u];
                        terminal_puts("usbports: ehci port=");
                        terminal_put_uint(port + 1u);
                        terminal_puts(" st=");
                        terminal_put_hex(st);
                        usb_dump_port_flags(st, 0);
                        terminal_putc('\n');
                    }
                }
            }
        }
    }
}

static unsigned int usb_power_ohci_controller(const pci_device_t* dev) {
    unsigned int bar = usb_controller_bar(dev);
    volatile u32* regs;
    u32 control;
    u32 desc_a;
    u32 rh_before;
    u32 rh_after;
    u32 ports;
    unsigned int powered = 0;
    unsigned short cmd;

    if (bar == 0) {
        terminal_puts("usbpower: ohci no-mmio-bar\n");
        return 0;
    }

    cmd = pci_read_config_word(dev->bus, dev->slot, dev->func, 0x04);
    pci_write_config_word(dev->bus, dev->slot, dev->func, 0x04,
                          (unsigned short)(cmd | PCI_COMMAND_MEMORY));

    usb_map_mmio(bar, PAGE_SIZE);
    regs = (volatile u32*)bar;

    control = regs[OHCI_HC_CONTROL / 4u];
    desc_a = regs[OHCI_HC_RH_DESC_A / 4u];
    rh_before = regs[OHCI_HC_RH_STATUS / 4u];
    ports = desc_a & 0xFFu;
    if (ports > 15u) ports = 15u;

    s_usb_debug.last_bus = dev->bus;
    s_usb_debug.last_slot = dev->slot;
    s_usb_debug.last_func = dev->func;
    s_usb_debug.last_prog_if = dev->prog_if;
    s_usb_debug.last_bar = bar;
    s_usb_debug.last_ports = ports;

    /*
     * This intentionally avoids EHCI routing and OHCI ownership/control-state
     * changes.  It only asks the OHCI root hub to switch port power on.
     */
    if (!(desc_a & OHCI_RHDA_NPS)) {
        if (desc_a & OHCI_RHDA_PSM) {
            for (u32 i = 0; i < ports; i++) {
                regs[(OHCI_HC_RH_PORT_BASE + i * 4u) / 4u] = OHCI_PORT_PPS;
            }
        } else {
            regs[OHCI_HC_RH_STATUS / 4u] = OHCI_RHS_LPSC;
        }
    }
    usb_delay();

    rh_after = regs[OHCI_HC_RH_STATUS / 4u];
    for (u32 i = 0; i < ports; i++) {
        u32 status = regs[(OHCI_HC_RH_PORT_BASE + i * 4u) / 4u];
        if (i == 0) s_usb_debug.last_port_status0 = status;
        if (i == 1) s_usb_debug.last_port_status1 = status;
        if ((desc_a & OHCI_RHDA_NPS) || (status & OHCI_PORT_PPS)) {
            powered++;
        }
    }
    s_usb_debug.powered_port_count = powered;

    terminal_puts("usbpower: ohci ");
    usb_print_addr(dev);
    terminal_puts(" ctrl=");
    terminal_put_hex(control);
    terminal_puts(" rhda=");
    terminal_put_hex(desc_a);
    terminal_puts(" rhs=");
    terminal_put_hex(rh_before);
    terminal_putc('/');
    terminal_put_hex(rh_after);
    terminal_puts(" ports=");
    terminal_put_uint(ports);
    terminal_puts(" pwr=");
    terminal_put_uint(powered);
    terminal_puts(" st=");
    terminal_put_hex(s_usb_debug.last_port_status0);
    terminal_putc('/');
    terminal_put_hex(s_usb_debug.last_port_status1);
    terminal_putc('\n');

    return powered;
}

unsigned int usb_power_ohci_ports(void) {
    unsigned int total_powered = 0;
    unsigned int ohci_seen = 0;

    if (s_usb_controller_count != 0u) {
        for (unsigned int i = 0; i < s_usb_controller_count; i++) {
            usb_controller_t* ctrl = &s_usb_controllers[i];
            if (!ctrl->present || ctrl->pci.prog_if != USB_PROG_OHCI) {
                continue;
            }
            ohci_seen++;
            total_powered += usb_power_ohci_controller(&ctrl->pci);
        }

        if (ohci_seen == 0) {
            terminal_puts("usbpower: no OHCI controller\n");
        }
        return total_powered;
    }

    for (unsigned int bus = 0; bus < 256u; bus++) {
        for (unsigned int slot = 0; slot < 32u; slot++) {
            unsigned short vendor = pci_read_config_word((unsigned char)bus,
                                                         (unsigned char)slot,
                                                         0,
                                                         0x00);
            if (vendor == 0xFFFFu) {
                continue;
            }

            unsigned char header_type = pci_read_config_byte((unsigned char)bus,
                                                             (unsigned char)slot,
                                                             0,
                                                             0x0E);
            unsigned int function_count = (header_type & 0x80u) ? 8u : 1u;

            for (unsigned int func = 0; func < function_count; func++) {
                pci_device_t dev;

                pci_read_device((unsigned char)bus,
                                (unsigned char)slot,
                                (unsigned char)func,
                                &dev);
                if (dev.vendor_id == 0xFFFFu ||
                    dev.class_code != PCI_CLASS_SERIAL_BUS ||
                    dev.subclass != PCI_SUBCLASS_USB ||
                    dev.prog_if != USB_PROG_OHCI) {
                    continue;
                }

                ohci_seen++;
                total_powered += usb_power_ohci_controller(&dev);
            }
        }
    }

    if (ohci_seen == 0) {
        terminal_puts("usbpower: no OHCI controller\n");
    }

    return total_powered;
}

static int ohci_td_done(const ohci_td_t* td) {
    return ((td->control >> OHCI_TD_CC_SHIFT) & 0xFu) != OHCI_TD_CC_NOT_ACCESSED;
}

static int ohci_td_ok(const ohci_td_t* td) {
    return ((td->control >> OHCI_TD_CC_SHIFT) & 0xFu) == 0u;
}

static void ohci_wait_frame(void) {
    unsigned int start = timer_get_ticks();
    unsigned int spins = 0;

    while (timer_get_ticks() == start && spins++ < 10000000u) {
        __asm__ __volatile__("" : : : "memory");
    }
}

static int ohci_wait_td(const ohci_td_t* td, unsigned int timeout_ms) {
    unsigned int deadline = timer_get_ticks() + timer_ms_to_ticks_round_up(timeout_ms);
    unsigned int spins = 0;

    while ((int)(timer_get_ticks() - deadline) < 0 && spins++ < 1000000u) {
        if (ohci_td_done(td)) {
            return ohci_td_ok(td);
        }
    }
    return 0;
}

static void ohci_prepare_control(volatile u32* regs, unsigned int addr,
                                 unsigned int low_speed, unsigned int mps) {
    ohci_quiet_interrupts(regs);
    if (s_ohci_verbose) {
        ohci_print_state(regs, "pre");
    }

    if (s_ohci_verbose) terminal_puts("usbmouse: prep-clear\n");
    k_memset(&s_control_ed, 0, sizeof(s_control_ed));
    k_memset(s_tds, 0, sizeof(s_tds));

    if (s_ohci_verbose) terminal_puts("usbmouse: prep-ed\n");
    s_control_ed.control =
        (addr & 0x7Fu) |
        OHCI_ED_CTRL_DIR_TD |
        (low_speed ? OHCI_ED_CTRL_SPEED_LOW : 0u) |
        ((mps & 0x7FFu) << OHCI_ED_CTRL_MPS_SHIFT);
    s_control_ed.head_td = (u32)(unsigned int)&s_tds[0];
    s_control_ed.tail_td = (u32)(unsigned int)&s_tds[3];
    s_control_ed.next_ed = 0;

    if (s_ohci_verbose) terminal_puts("usbmouse: prep-head\n");
    regs[OHCI_HC_CONTROL_HEAD_REG / 4u] = (u32)(unsigned int)&s_control_ed;
    if (s_ohci_verbose) terminal_puts("usbmouse: prep-cur\n");
    regs[OHCI_HC_CONTROL_CUR_REG / 4u] = 0;
    if (s_ohci_verbose) terminal_puts("usbmouse: prep-enable\n");
    regs[OHCI_HC_CONTROL / 4u] =
        (regs[OHCI_HC_CONTROL / 4u] | OHCI_CONTROL_CLE | OHCI_CONTROL_USB_OPERATIONAL);
    if (s_ohci_verbose) {
        ohci_print_state(regs, "prepared");
        terminal_puts("usbmouse: prep-done\n");
    }
}

static int ohci_control_transfer(volatile u32* regs,
                                 unsigned int addr,
                                 unsigned int low_speed,
                                 unsigned int mps,
                                 u8 bm_request_type,
                                 u8 b_request,
                                 unsigned short w_value,
                                 unsigned short w_index,
                                 unsigned short w_length,
                                 void* data,
                                 unsigned int* inout_len) {
    unsigned int data_len = inout_len ? *inout_len : 0u;
    unsigned int has_data = w_length != 0u && data != 0 && data_len != 0u;
    unsigned int status_dp = (bm_request_type & 0x80u) ? OHCI_TD_DP_OUT : OHCI_TD_DP_IN;
    unsigned int data_dp = (bm_request_type & 0x80u) ? OHCI_TD_DP_IN : OHCI_TD_DP_OUT;
    ohci_td_t* tail;

    if (s_ohci_verbose) {
        terminal_puts("usbmouse: xfer req=");
        terminal_put_hex(b_request);
        terminal_puts(" addr=");
        terminal_put_uint(addr);
        terminal_puts(" len=");
        terminal_put_uint(w_length);
        terminal_putc('\n');
    }

    if (data_len > w_length) {
        data_len = w_length;
    }

    s_setup.bm_request_type = bm_request_type;
    s_setup.b_request = b_request;
    s_setup.w_value = w_value;
    s_setup.w_index = w_index;
    s_setup.w_length = w_length;

    ohci_prepare_control(regs, addr, low_speed, mps);

    s_tds[0].control = (OHCI_TD_CC_NOT_ACCESSED << OHCI_TD_CC_SHIFT) |
                       OHCI_TD_DP_SETUP | OHCI_TD_DI_NONE | OHCI_TD_TOGGLE_DATA0;
    s_tds[0].cbp = (u32)(unsigned int)&s_setup;
    s_tds[0].be = (u32)(unsigned int)&s_setup + sizeof(s_setup) - 1u;
    s_tds[0].next_td = (u32)(unsigned int)&s_tds[1];

    if (has_data) {
        s_tds[1].control = (OHCI_TD_CC_NOT_ACCESSED << OHCI_TD_CC_SHIFT) |
                           data_dp | OHCI_TD_DI_NONE | OHCI_TD_TOGGLE_DATA1;
        s_tds[1].cbp = (u32)(unsigned int)data;
        s_tds[1].be = (u32)(unsigned int)data + data_len - 1u;
        s_tds[1].next_td = (u32)(unsigned int)&s_tds[2];

        s_tds[2].control = (OHCI_TD_CC_NOT_ACCESSED << OHCI_TD_CC_SHIFT) |
                           status_dp | OHCI_TD_DI_NONE | OHCI_TD_TOGGLE_DATA1;
        s_tds[2].cbp = 0;
        s_tds[2].be = 0;
        s_tds[2].next_td = (u32)(unsigned int)&s_tds[3];
        tail = &s_tds[2];
    } else {
        s_tds[1].control = (OHCI_TD_CC_NOT_ACCESSED << OHCI_TD_CC_SHIFT) |
                           status_dp | OHCI_TD_DI_NONE | OHCI_TD_TOGGLE_DATA1;
        s_tds[1].cbp = 0;
        s_tds[1].be = 0;
        s_tds[1].next_td = (u32)(unsigned int)&s_tds[3];
        tail = &s_tds[1];
    }

    if (s_ohci_verbose) {
        terminal_puts("usbmouse: kick req=");
        terminal_put_hex(b_request);
        terminal_puts(" ed=");
        terminal_put_hex((u32)(unsigned int)&s_control_ed);
        terminal_puts(" td=");
        terminal_put_hex((u32)(unsigned int)&s_tds[0]);
        terminal_putc('\n');
    }

    if (s_ohci_dry_run) {
        if (s_usb_mouse_log) {
            terminal_puts("usbmouse: dry-run no kick\n");
        }
        return 0;
    }

    regs[OHCI_HC_COMMAND_STATUS / 4u] = OHCI_CMD_CLF;
    if (s_ohci_verbose) {
        terminal_puts("usbmouse: kicked\n");
        ohci_print_state(regs, "postkick");
    }
    if (!ohci_wait_td(tail, b_request == USB_REQ_SET_ADDRESS ? 100u : 1000u)) {
        if (s_usb_mouse_log) {
            terminal_puts("usbmouse: control fail req=");
            terminal_put_hex(b_request);
            terminal_puts(" cc=");
            terminal_put_hex((tail->control >> OHCI_TD_CC_SHIFT) & 0xFu);
            terminal_puts(" td=");
            terminal_put_hex(s_tds[0].control);
            terminal_putc('/');
            terminal_put_hex(s_tds[1].control);
            terminal_putc('/');
            terminal_put_hex(s_tds[2].control);
            terminal_puts(" head=");
            terminal_put_hex(s_control_ed.head_td);
            terminal_puts(" tail=");
            terminal_put_hex(s_control_ed.tail_td);
            terminal_puts(" stat=");
            terminal_put_hex(regs[OHCI_HC_COMMAND_STATUS / 4u]);
            terminal_putc('\n');
        }
        regs[OHCI_HC_CONTROL / 4u] = regs[OHCI_HC_CONTROL / 4u] & ~OHCI_CONTROL_CLE;
        s_control_ed.control |= OHCI_ED_CTRL_SKIP;
        return 0;
    }

    if (inout_len) {
        if (has_data && s_tds[1].cbp != 0) {
            unsigned int remain = s_tds[1].be - s_tds[1].cbp + 1u;
            *inout_len = data_len - remain;
        } else {
            *inout_len = data_len;
        }
    }

    return 1;
}

static int usb_parse_hid_config(const u8* data,
                                unsigned int len,
                                usb_mouse_dev_t* out,
                                unsigned int desired_protocol) {
    unsigned int i = 0;
    int in_hid_interface = 0;
    unsigned int protocol = 0;

    while (i + 2u <= len) {
        unsigned int dlen = data[i];
        unsigned int dtype = data[i + 1u];
        if (dlen < 2u || i + dlen > len) {
            break;
        }

        if (dtype == 4u && dlen >= 9u) {
            protocol = data[i + 7u];
            in_hid_interface =
                data[i + 5u] == USB_CLASS_HID &&
                data[i + 6u] == USB_HID_SUBCLASS_BOOT &&
                (protocol == USB_HID_PROTOCOL_KEYBOARD ||
                 protocol == USB_HID_PROTOCOL_MOUSE) &&
                (desired_protocol == 0u || protocol == desired_protocol);
            if (in_hid_interface) {
                out->interface_number = data[i + 2u];
                out->protocol = protocol;
            }
        } else if (dtype == 5u && dlen >= 7u && in_hid_interface) {
            if ((data[i + 2u] & USB_ENDPOINT_DIR_IN) &&
                ((data[i + 3u] & 0x03u) == USB_ENDPOINT_TYPE_INTERRUPT)) {
                out->endpoint = data[i + 2u] & 0x0Fu;
                out->packet_size = (unsigned int)data[i + 4u] |
                                   ((unsigned int)data[i + 5u] << 8);
                if (out->packet_size == 0 || out->packet_size > sizeof(s_report_buf)) {
                    out->packet_size = sizeof(s_report_buf);
                }
                out->interval = data[i + 6u] ? data[i + 6u] : 10u;
                return 1;
            }
        }

        i += dlen;
    }

    return 0;
}

static int ohci_reset_port(volatile u32* regs, unsigned int port, unsigned int* low_speed) {
    volatile u32* port_reg = &regs[(OHCI_HC_RH_PORT_BASE + port * 4u) / 4u];
    u32 status = *port_reg;
    u32 before;
    unsigned int deadline;
    unsigned int spins = 0;

    if (!(status & OHCI_PORT_CCS)) {
        return 0;
    }

    before = status;
    *port_reg = OHCI_PORT_PRS;
    deadline = timer_get_ticks() + timer_ms_to_ticks_round_up(150u);
    while ((int)(timer_get_ticks() - deadline) < 0 && spins++ < 10000000u) {
        status = *port_reg;
        if (status & OHCI_PORT_PRSC) {
            break;
        }
    }
    *port_reg = OHCI_PORT_PRSC;
    usb_delay();

    status = *port_reg;
    if (s_usb_mouse_log) {
        terminal_puts("usbmouse: reset p=");
        terminal_put_uint(port + 1u);
        terminal_puts(" st=");
        terminal_put_hex(before);
        terminal_puts("->");
        terminal_put_hex(status);
        terminal_puts(" low=");
        terminal_put_uint((status & OHCI_PORT_LSDA) ? 1u : 0u);
        terminal_putc('\n');
    }

    if (!(status & OHCI_PORT_PES)) {
        if (s_usb_mouse_log) {
            terminal_puts("usbmouse: port reset not enabled st=");
            terminal_put_hex(status);
            terminal_putc('\n');
        }
        return 0;
    }

    *low_speed = (status & OHCI_PORT_LSDA) ? 1u : 0u;
    return 1;
}

static int usb_try_hid_on_port(volatile u32* regs,
                               unsigned int port,
                               usb_mouse_dev_t* out,
                               unsigned int desired_protocol,
                               unsigned int address);

static int usb_try_mouse_on_port(volatile u32* regs, unsigned int port, usb_mouse_dev_t* out) {
    return usb_try_hid_on_port(regs, port, out, USB_HID_PROTOCOL_MOUSE, 1u);
}

static int usb_try_hid_on_port(volatile u32* regs,
                               unsigned int port,
                               usb_mouse_dev_t* out,
                               unsigned int desired_protocol,
                               unsigned int address) {
    unsigned int low_speed = 0;
    unsigned int len;
    unsigned int mps = 8;
    unsigned int total_len;
    unsigned int config_value;

    if (!ohci_reset_port(regs, port, &low_speed)) {
        return 0;
    }

    k_memset(s_usb_buf, 0, sizeof(s_usb_buf));
    len = 8u;
    if (s_usb_mouse_log) terminal_puts("usbmouse: getdev8... ");
    if (!ohci_control_transfer(regs, 0, low_speed, 8u,
                               0x80u, USB_REQ_GET_DESCRIPTOR,
                               (USB_DESC_DEVICE << 8), 0, 8u,
                               s_usb_buf, &len)) {
        if (s_usb_mouse_log) terminal_puts("fail\n");
        return 0;
    }
    if (s_usb_mouse_log) {
        terminal_puts("ok len=");
        terminal_put_uint(len);
        terminal_puts(" mps=");
        terminal_put_uint(s_usb_buf[7u]);
        terminal_putc('\n');
    }
    mps = s_usb_buf[7u] ? s_usb_buf[7u] : 8u;
    if (mps > 64u) mps = 8u;

    if (s_usb_mouse_log) terminal_puts("usbmouse: setaddr... ");
    if (address == 0u || address > 127u) {
        return 0;
    }

    if (!ohci_control_transfer(regs, 0, low_speed, mps,
                               0x00u, USB_REQ_SET_ADDRESS,
                               (unsigned short)address, 0, 0, 0, 0)) {
        if (s_usb_mouse_log) terminal_puts("fail\n");
        return 0;
    }
    if (s_usb_mouse_log) terminal_puts("ok\n");
    for (unsigned int i = 0; i < 3u; i++) {
        ohci_wait_frame();
    }

    k_memset(s_usb_buf, 0, sizeof(s_usb_buf));
    len = 18u;
    if (s_usb_mouse_log) terminal_puts("usbmouse: getdev18... ");
    if (!ohci_control_transfer(regs, address, low_speed, mps,
                               0x80u, USB_REQ_GET_DESCRIPTOR,
                               (USB_DESC_DEVICE << 8), 0, 18u,
                               s_usb_buf, &len)) {
        if (s_usb_mouse_log) terminal_puts("fail\n");
        return 0;
    }
    if (s_usb_mouse_log) {
        terminal_puts("ok len=");
        terminal_put_uint(len);
        if (len >= 12u) {
            unsigned int vendor = (unsigned int)s_usb_buf[8u] | ((unsigned int)s_usb_buf[9u] << 8);
            unsigned int product = (unsigned int)s_usb_buf[10u] | ((unsigned int)s_usb_buf[11u] << 8);
            terminal_puts(" vid=");
            terminal_put_hex(vendor);
            terminal_puts(" pid=");
            terminal_put_hex(product);
        }
        terminal_putc('\n');
    }

    k_memset(s_usb_buf, 0, sizeof(s_usb_buf));
    len = 9u;
    if (s_usb_mouse_log) terminal_puts("usbmouse: getcfg9... ");
    if (!ohci_control_transfer(regs, address, low_speed, mps,
                               0x80u, USB_REQ_GET_DESCRIPTOR,
                               (USB_DESC_CONFIGURATION << 8), 0, 9u,
                               s_usb_buf, &len)) {
        if (s_usb_mouse_log) terminal_puts("fail\n");
        return 0;
    }
    total_len = (unsigned int)s_usb_buf[2u] | ((unsigned int)s_usb_buf[3u] << 8);
    config_value = s_usb_buf[5u];
    if (total_len > sizeof(s_usb_buf)) total_len = sizeof(s_usb_buf);
    if (s_usb_mouse_log) {
        terminal_puts("ok total=");
        terminal_put_uint(total_len);
        terminal_puts(" cfg=");
        terminal_put_uint(config_value);
        terminal_putc('\n');
    }

    k_memset(s_usb_buf, 0, sizeof(s_usb_buf));
    len = total_len;
    if (s_usb_mouse_log) {
        terminal_puts("usbmouse: getcfgfull... len=");
        terminal_put_uint(total_len);
        terminal_puts(" ");
    }
    if (!ohci_control_transfer(regs, address, low_speed, mps,
                               0x80u, USB_REQ_GET_DESCRIPTOR,
                               (USB_DESC_CONFIGURATION << 8), 0,
                               (unsigned short)total_len,
                               s_usb_buf, &len)) {
        if (s_usb_mouse_log) terminal_puts("fail\n");
        return 0;
    }
    if (s_usb_mouse_log) {
        terminal_puts("ok got=");
        terminal_put_uint(len);
        terminal_putc('\n');
    }

    k_memset(out, 0, sizeof(*out));
    out->regs = regs;
    out->port_index = port;
    out->low_speed = low_speed;
    out->address = address;
    if (!usb_parse_hid_config(s_usb_buf, len, out, desired_protocol)) {
        if (s_usb_mouse_log) {
            terminal_puts("usbmouse: no boot HID match on port ");
            terminal_put_uint(port + 1u);
            terminal_putc('\n');
        }
        return 0;
    }

    if (s_usb_mouse_log) terminal_puts("usbmouse: setcfg... ");
    if (!ohci_control_transfer(regs, address, low_speed, mps,
                               0x00u, USB_REQ_SET_CONFIGURATION,
                               (unsigned short)config_value, 0, 0, 0, 0)) {
        if (s_usb_mouse_log) terminal_puts("fail\n");
        return 0;
    }
    if (s_usb_mouse_log) terminal_puts("ok\n");

    if (s_usb_mouse_log) terminal_puts("usbmouse: hid init... ");
    (void)ohci_control_transfer(regs, address, low_speed, mps,
                                0x21u, USB_REQ_SET_PROTOCOL,
                                0, (unsigned short)out->interface_number,
                                0, 0, 0);
    (void)ohci_control_transfer(regs, address, low_speed, mps,
                                0x21u, USB_REQ_SET_IDLE,
                                0, (unsigned short)out->interface_number,
                                0, 0, 0);
    if (s_usb_mouse_log) terminal_puts("done\n");
    for (unsigned int i = 0; i < 20u; i++) {
        ohci_wait_frame();
    }

    if (s_usb_mouse_log) {
        terminal_puts("usbmouse: mouse port=");
        terminal_put_uint(port + 1u);
        terminal_puts(" low=");
        terminal_put_uint(low_speed);
        terminal_puts(" ep=");
        terminal_put_uint(out->endpoint);
        terminal_puts(" pkt=");
        terminal_put_uint(out->packet_size);
        terminal_puts(" int=");
        terminal_put_uint(out->interval);
        terminal_puts(" proto=");
        terminal_put_uint(out->protocol);
        terminal_putc('\n');
    }
    return 1;
}

static void usb_print_device_descriptor(const u8* data, unsigned int len) {
    if (len < 8u) {
        terminal_puts("usbpeek: short descriptor len=");
        terminal_put_uint(len);
        terminal_putc('\n');
        return;
    }

    terminal_puts("usbpeek: dev len=");
    terminal_put_uint(len);
    terminal_puts(" class=");
    terminal_put_hex(data[4u]);
    terminal_puts(" sub=");
    terminal_put_hex(data[5u]);
    terminal_puts(" proto=");
    terminal_put_hex(data[6u]);
    terminal_puts(" mps=");
    terminal_put_uint(data[7u]);
    if (len >= 12u) {
        unsigned int vendor = (unsigned int)data[8u] | ((unsigned int)data[9u] << 8);
        unsigned int product = (unsigned int)data[10u] | ((unsigned int)data[11u] << 8);
        terminal_puts(" vid=");
        terminal_put_hex(vendor);
        terminal_puts(" pid=");
        terminal_put_hex(product);
    }
    terminal_putc('\n');
}

static void usb_peek_ohci_port(volatile u32* regs, unsigned int port) {
    unsigned int low_speed = 0;
    unsigned int len;

    ohci_quiet_interrupts(regs);
    ohci_bind_hcca(regs);

    if (s_ohci_dry_run) {
        u32 st = regs[(OHCI_HC_RH_PORT_BASE + port * 4u) / 4u];
        terminal_puts("usbpeek: dry-run port=");
        terminal_put_uint(port + 1u);
        terminal_puts(" st=");
        terminal_put_hex(st);
        terminal_puts(" no-reset no-dma\n");
        return;
    }

    if (!ohci_reset_port(regs, port, &low_speed)) {
        terminal_puts("usbpeek: reset failed port=");
        terminal_put_uint(port + 1u);
        terminal_putc('\n');
        return;
    }

    terminal_puts("usbpeek: port=");
    terminal_put_uint(port + 1u);
    terminal_puts(" low=");
    terminal_put_uint(low_speed);
    terminal_putc('\n');

    k_memset(s_usb_buf, 0, sizeof(s_usb_buf));
    len = 8u;
    if (!ohci_control_transfer(regs, 0, low_speed, 8u,
                               0x80u, USB_REQ_GET_DESCRIPTOR,
                               (USB_DESC_DEVICE << 8), 0, 8u,
                               s_usb_buf, &len)) {
        terminal_puts("usbpeek: getdev8 failed\n");
        return;
    }
    usb_print_device_descriptor(s_usb_buf, len);

    /*
     * Many devices allow the full descriptor at address 0 after reset.  If
     * not, this should fail and return without assigning an address.
     */
    k_memset(s_usb_buf, 0, sizeof(s_usb_buf));
    len = 18u;
    if (ohci_control_transfer(regs, 0, low_speed, s_usb_buf[7u] ? s_usb_buf[7u] : 8u,
                              0x80u, USB_REQ_GET_DESCRIPTOR,
                              (USB_DESC_DEVICE << 8), 0, 18u,
                              s_usb_buf, &len)) {
        usb_print_device_descriptor(s_usb_buf, len);
    } else {
        terminal_puts("usbpeek: getdev18 failed\n");
    }
}

void usb_peek_port(unsigned int one_based_port) {
    terminal_puts("usbpeek: OHCI port=");
    terminal_put_uint(one_based_port);
    terminal_putc('\n');

    for (unsigned int bus = 0; bus < 256u; bus++) {
        for (unsigned int slot = 0; slot < 32u; slot++) {
            unsigned short vendor = pci_read_config_word((unsigned char)bus,
                                                         (unsigned char)slot,
                                                         0,
                                                         0x00);
            if (vendor == 0xFFFFu) {
                continue;
            }

            unsigned char header_type = pci_read_config_byte((unsigned char)bus,
                                                             (unsigned char)slot,
                                                             0,
                                                             0x0E);
            unsigned int function_count = (header_type & 0x80u) ? 8u : 1u;

            for (unsigned int func = 0; func < function_count; func++) {
                pci_device_t dev;
                unsigned int bar;
                volatile u32* regs;
                u32 ports;

                pci_read_device((unsigned char)bus,
                                (unsigned char)slot,
                                (unsigned char)func,
                                &dev);
                if (dev.vendor_id == 0xFFFFu ||
                    dev.class_code != PCI_CLASS_SERIAL_BUS ||
                    dev.subclass != PCI_SUBCLASS_USB ||
                    dev.prog_if != USB_PROG_OHCI) {
                    continue;
                }

                bar = usb_controller_bar(&dev);
                if (bar == 0) {
                    continue;
                }
                usb_map_mmio(bar, PAGE_SIZE);
                regs = (volatile u32*)bar;
                ohci_quiet_interrupts(regs);
                ohci_bind_hcca(regs);
                ports = regs[OHCI_HC_RH_DESC_A / 4u] & 0xFFu;
                if (one_based_port == 0u || one_based_port > ports) {
                    terminal_puts("usbpeek: invalid port\n");
                    return;
                }
                usb_peek_ohci_port(regs, one_based_port - 1u);
                return;
            }
        }
    }

    terminal_puts("usbpeek: no OHCI controller\n");
}

static void usb_diag_peek_ohci_candidates(void) {
    terminal_puts("usbdiag: dry-run connected non-low OHCI ports\n");

    for (unsigned int bus = 0; bus < 256u; bus++) {
        for (unsigned int slot = 0; slot < 32u; slot++) {
            unsigned short vendor = pci_read_config_word((unsigned char)bus,
                                                         (unsigned char)slot,
                                                         0,
                                                         0x00);
            if (vendor == 0xFFFFu) {
                continue;
            }

            unsigned char header_type = pci_read_config_byte((unsigned char)bus,
                                                             (unsigned char)slot,
                                                             0,
                                                             0x0E);
            unsigned int function_count = (header_type & 0x80u) ? 8u : 1u;

            for (unsigned int func = 0; func < function_count; func++) {
                pci_device_t dev;
                unsigned int bar;
                volatile u32* regs;
                u32 ports;
                unsigned int candidates = 0;

                pci_read_device((unsigned char)bus,
                                (unsigned char)slot,
                                (unsigned char)func,
                                &dev);
                if (dev.vendor_id == 0xFFFFu ||
                    dev.class_code != PCI_CLASS_SERIAL_BUS ||
                    dev.subclass != PCI_SUBCLASS_USB ||
                    dev.prog_if != USB_PROG_OHCI) {
                    continue;
                }

                bar = usb_controller_bar(&dev);
                if (bar == 0) {
                    continue;
                }
                usb_map_mmio(bar, PAGE_SIZE);
                regs = (volatile u32*)bar;
                ports = regs[OHCI_HC_RH_DESC_A / 4u] & 0xFFu;
                if (ports > 15u) ports = 15u;

                for (u32 port = 0; port < ports; port++) {
                    u32 st = regs[(OHCI_HC_RH_PORT_BASE + port * 4u) / 4u];
                    if (!(st & OHCI_PORT_CCS)) {
                        continue;
                    }
                    if (st & OHCI_PORT_LSDA) {
                        terminal_puts("usbdiag: skip low-speed port=");
                        terminal_put_uint(port + 1u);
                        terminal_puts(" st=");
                        terminal_put_hex(st);
                        terminal_putc('\n');
                        continue;
                    }
                    candidates++;
                    terminal_puts("usbdiag: peek port=");
                    terminal_put_uint(port + 1u);
                    terminal_puts(" st=");
                    terminal_put_hex(st);
                    terminal_puts(" dry-run no-reset no-dma\n");
                }

                if (candidates == 0) {
                    terminal_puts("usbdiag: no non-low OHCI candidates\n");
                }
                return;
            }
        }
    }

    terminal_puts("usbdiag: no OHCI controller\n");
}

void usb_diag(void) {
    terminal_puts("usbdiag: begin\n");
    usb_dump_ports();

    usb_diag_peek_ohci_candidates();
    s_ohci_dry_run = 0;
    terminal_puts("usbdiag: done\n");
}

static int usb_find_ohci_mouse(usb_mouse_dev_t* out, unsigned int one_based_port) {
    unsigned int ohci_count = 0;

    if (s_usb_mouse_log) {
        terminal_puts("usbmouse: scan start usb-hid-diag1\n");
    }

    if (s_usb_controller_count != 0u) {
        for (unsigned int i = 0; i < s_usb_controller_count; i++) {
            usb_controller_t* ctrl = &s_usb_controllers[i];
            volatile u32* regs;
            u32 ports;
            ohci_saved_state_t saved;

            if (!ctrl->present || ctrl->pci.prog_if != USB_PROG_OHCI) {
                continue;
            }

            ohci_count++;
            regs = ctrl->regs;
            if (s_usb_mouse_log) {
                terminal_puts("usbmouse: ohci ");
                usb_print_addr(&ctrl->pci);
                terminal_puts(" bar=");
                terminal_put_hex(ctrl->bar);
                terminal_putc('\n');
            }
            if (!regs) {
                continue;
            }

            ohci_save_state(regs, &saved);
            ohci_quiet_interrupts(regs);
            regs[OHCI_HC_CONTROL / 4u] =
                (regs[OHCI_HC_CONTROL / 4u] & ~OHCI_CONTROL_PLE) |
                OHCI_CONTROL_CLE | OHCI_CONTROL_USB_OPERATIONAL;
            ohci_bind_hcca(regs);

            ports = ctrl->ports;
            if (ports == 0u) {
                ports = regs[OHCI_HC_RH_DESC_A / 4u] & 0xFFu;
                if (ports > 15u) ports = 15u;
            }

            for (u32 port = 0; port < ports; port++) {
                u32 st = regs[(OHCI_HC_RH_PORT_BASE + port * 4u) / 4u];
                if (one_based_port != 0u && one_based_port != port + 1u) {
                    continue;
                }
                if (s_usb_mouse_log) {
                    terminal_puts("usbmouse: port=");
                    terminal_put_uint(port + 1u);
                    terminal_puts(" st=");
                    terminal_put_hex(st);
                    terminal_putc('\n');
                }
                if (!(st & OHCI_PORT_CCS)) {
                    continue;
                }
                if (usb_try_mouse_on_port(regs, port, out)) {
                    out->bar = ctrl->bar;
                    out->has_saved_state = 1;
                    out->saved_state = saved;
                    return 1;
                }
            }
            ohci_restore_state(regs, &saved);
        }

        if (s_usb_mouse_log) {
            terminal_puts("usbmouse: ohci_count=");
            terminal_put_uint(ohci_count);
            terminal_putc('\n');
        }
        return 0;
    }

    for (unsigned int bus = 0; bus < 256u; bus++) {
        for (unsigned int slot = 0; slot < 32u; slot++) {
            unsigned short vendor = pci_read_config_word((unsigned char)bus,
                                                         (unsigned char)slot,
                                                         0,
                                                         0x00);
            if (vendor == 0xFFFFu) {
                continue;
            }

            unsigned char header_type = pci_read_config_byte((unsigned char)bus,
                                                             (unsigned char)slot,
                                                             0,
                                                             0x0E);
            unsigned int function_count = (header_type & 0x80u) ? 8u : 1u;

            for (unsigned int func = 0; func < function_count; func++) {
                pci_device_t dev;
                unsigned int bar;
                volatile u32* regs;
                u32 desc_a;
                u32 ports;
                ohci_saved_state_t saved;

                pci_read_device((unsigned char)bus,
                                (unsigned char)slot,
                                (unsigned char)func,
                                &dev);
                if (dev.vendor_id == 0xFFFFu ||
                    dev.class_code != PCI_CLASS_SERIAL_BUS ||
                    dev.subclass != PCI_SUBCLASS_USB ||
                    dev.prog_if != USB_PROG_OHCI) {
                    continue;
                }

                ohci_count++;
                bar = usb_controller_bar(&dev);
                if (s_usb_mouse_log) {
                    terminal_puts("usbmouse: ohci ");
                    usb_print_addr(&dev);
                    terminal_puts(" bar=");
                    terminal_put_hex(bar);
                    terminal_putc('\n');
                }
                if (bar == 0) {
                    continue;
                }
                usb_map_mmio(bar, PAGE_SIZE);
                regs = (volatile u32*)bar;
                ohci_save_state(regs, &saved);
                ohci_quiet_interrupts(regs);
                regs[OHCI_HC_CONTROL / 4u] =
                    (regs[OHCI_HC_CONTROL / 4u] & ~OHCI_CONTROL_PLE) |
                    OHCI_CONTROL_CLE | OHCI_CONTROL_USB_OPERATIONAL;
                ohci_bind_hcca(regs);

                desc_a = regs[OHCI_HC_RH_DESC_A / 4u];
                ports = desc_a & 0xFFu;
                if (ports > 15u) ports = 15u;

                for (u32 port = 0; port < ports; port++) {
                    u32 st = regs[(OHCI_HC_RH_PORT_BASE + port * 4u) / 4u];
                    if (one_based_port != 0u && one_based_port != port + 1u) {
                        continue;
                    }
                    if (s_usb_mouse_log) {
                        terminal_puts("usbmouse: port=");
                        terminal_put_uint(port + 1u);
                        terminal_puts(" st=");
                        terminal_put_hex(st);
                        terminal_putc('\n');
                    }
                    if (!(st & OHCI_PORT_CCS)) {
                        continue;
                    }
                    if (usb_try_mouse_on_port(regs, port, out)) {
                        out->bar = bar;
                        out->has_saved_state = 1;
                        out->saved_state = saved;
                        return 1;
                    }
                }
                ohci_restore_state(regs, &saved);
            }
        }
    }

    if (s_usb_mouse_log) {
        terminal_puts("usbmouse: ohci_count=");
        terminal_put_uint(ohci_count);
        terminal_putc('\n');
    }
    return 0;
}

static void usb_init_interrupt_ed(const usb_mouse_dev_t* dev,
                                  ohci_ed_t* ed,
                                  ohci_td_t* td) {
    k_memset(ed, 0, sizeof(*ed));
    k_memset(td, 0, sizeof(ohci_td_t) * 2u);

    ed->control =
        (dev->address & 0x7Fu) |
        ((dev->endpoint & 0xFu) << 7u) |
        OHCI_ED_CTRL_DIR_IN |
        (dev->low_speed ? OHCI_ED_CTRL_SPEED_LOW : 0u) |
        ((dev->packet_size & 0x7FFu) << OHCI_ED_CTRL_MPS_SHIFT);
    ed->head_td = (u32)(unsigned int)&td[0];
    ed->tail_td = (u32)(unsigned int)&td[1];
    ed->next_ed = 0;
}

static volatile u32* usb_active_ohci_regs(void) {
    if (s_usb_keyboard_active) {
        return s_usb_keyboard_active_dev.regs;
    }
    if (s_usb_mouse_active) {
        return s_usb_mouse_active_dev.regs;
    }
    return 0;
}

static void usb_rebuild_periodic_schedule(void) {
    volatile u32* regs = usb_active_ohci_regs();
    ohci_ed_t* first = 0;
    ohci_ed_t* second = 0;

    if (!regs) {
        return;
    }

    k_memset(&s_ohci_hcca, 0, sizeof(s_ohci_hcca));

    if (s_usb_keyboard_active) {
        first = &s_keyboard_intr_ed;
    }
    if (s_usb_mouse_active) {
        if (!first) {
            first = &s_intr_ed;
        } else {
            second = &s_intr_ed;
        }
    }

    if (!first) {
        regs[OHCI_HC_CONTROL / 4u] =
            regs[OHCI_HC_CONTROL / 4u] & ~OHCI_CONTROL_PLE;
        return;
    }

    first->next_ed = second ? (u32)(unsigned int)second : 0u;
    if (second) {
        second->next_ed = 0;
    }

    for (unsigned int i = 0; i < 32u; i++) {
        s_ohci_hcca.interrupt_table[i] = (u32)(unsigned int)first;
    }

    regs[OHCI_HC_HCCA / 4u] = (u32)(unsigned int)&s_ohci_hcca;
    regs[OHCI_HC_CONTROL / 4u] =
        regs[OHCI_HC_CONTROL / 4u] | OHCI_CONTROL_PLE | OHCI_CONTROL_USB_OPERATIONAL;
}

static void usb_mouse_setup_interrupt(const usb_mouse_dev_t* dev) {
    unsigned int interval = dev->interval ? dev->interval : 10u;
    u32 fm_interval;
    u32 frame_interval;
    u32 periodic_start;

    if (interval > 32u) {
        interval = 32u;
    }

    usb_init_interrupt_ed(dev, &s_intr_ed, s_intr_td);
    fm_interval = dev->regs[OHCI_HC_FM_INTERVAL / 4u];
    frame_interval = fm_interval & 0x3FFFu;
    if (frame_interval != 0u) {
        periodic_start = (frame_interval * 9u) / 10u;
        dev->regs[OHCI_HC_PERIOD_START / 4u] = periodic_start;
    } else {
        periodic_start = dev->regs[OHCI_HC_PERIOD_START / 4u];
    }
    (void)interval;
    usb_rebuild_periodic_schedule();

    if (s_usb_mouse_log) {
        terminal_puts("usbmouse: intr setup int=");
        terminal_put_uint(interval);
        terminal_puts(" fm=");
        terminal_put_hex(fm_interval);
        terminal_puts(" ps=");
        terminal_put_hex(periodic_start);
        terminal_puts(" ctrl=");
        terminal_put_hex(dev->regs[OHCI_HC_CONTROL / 4u]);
        terminal_putc('\n');
    }
}

static void usb_mouse_queue_intr_td(const usb_mouse_dev_t* dev) {
    unsigned int report_len = dev->packet_size;
    u32 toggle = s_intr_ed.head_td & OHCI_ED_HEAD_TOGGLE;

    if (report_len == 0u || report_len > sizeof(s_report_buf)) {
        report_len = sizeof(s_report_buf);
    }

    k_memset(s_report_buf, 0, sizeof(s_report_buf));
    s_intr_td[0].control = (OHCI_TD_CC_NOT_ACCESSED << OHCI_TD_CC_SHIFT) |
                           OHCI_TD_BUFFER_ROUNDING | OHCI_TD_DP_IN |
                           OHCI_TD_DI_NONE | OHCI_TD_TOGGLE_CARRY;
    s_intr_td[0].cbp = (u32)(unsigned int)s_report_buf;
    s_intr_td[0].be = (u32)(unsigned int)s_report_buf + report_len - 1u;
    s_intr_td[0].next_td = (u32)(unsigned int)&s_intr_td[1];
    s_intr_ed.head_td = ((u32)(unsigned int)&s_intr_td[0]) | toggle;
    s_intr_ed.tail_td = (u32)(unsigned int)&s_intr_td[1];
}

static void usb_keyboard_queue_intr_td(const usb_mouse_dev_t* dev) {
    unsigned int report_len = dev->packet_size;
    u32 toggle = s_keyboard_intr_ed.head_td & OHCI_ED_HEAD_TOGGLE;

    if (report_len == 0u || report_len > sizeof(s_keyboard_report_buf)) {
        report_len = sizeof(s_keyboard_report_buf);
    }

    k_memset(s_keyboard_report_buf, 0, sizeof(s_keyboard_report_buf));
    s_keyboard_intr_td[0].control = (OHCI_TD_CC_NOT_ACCESSED << OHCI_TD_CC_SHIFT) |
                                    OHCI_TD_BUFFER_ROUNDING | OHCI_TD_DP_IN |
                                    OHCI_TD_DI_NONE | OHCI_TD_TOGGLE_CARRY;
    s_keyboard_intr_td[0].cbp = (u32)(unsigned int)s_keyboard_report_buf;
    s_keyboard_intr_td[0].be = (u32)(unsigned int)s_keyboard_report_buf + report_len - 1u;
    s_keyboard_intr_td[0].next_td = (u32)(unsigned int)&s_keyboard_intr_td[1];
    s_keyboard_intr_ed.head_td = ((u32)(unsigned int)&s_keyboard_intr_td[0]) | toggle;
    s_keyboard_intr_ed.tail_td = (u32)(unsigned int)&s_keyboard_intr_td[1];
}

static unsigned char usb_hid_usage_to_set1(unsigned int usage, int* e0) {
    static const unsigned char letters[26] = {
        0x1E, 0x30, 0x2E, 0x20, 0x12, 0x21, 0x22, 0x23, 0x17, 0x24, 0x25, 0x26, 0x32,
        0x31, 0x18, 0x19, 0x10, 0x13, 0x1F, 0x14, 0x16, 0x2F, 0x11, 0x2D, 0x15, 0x2C
    };
    static const unsigned char digits[10] = {
        0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B
    };

    *e0 = 0;

    if (usage >= 0x04u && usage <= 0x1Du) {
        return letters[usage - 0x04u];
    }
    if (usage >= 0x1Eu && usage <= 0x27u) {
        return digits[usage - 0x1Eu];
    }
    if (usage >= 0x3Au && usage <= 0x43u) {
        return (unsigned char)(0x3Bu + (usage - 0x3Au));
    }

    switch (usage) {
        case 0x28u: return 0x1C; /* Enter */
        case 0x29u: return 0x01; /* Escape */
        case 0x2Au: return 0x0E; /* Backspace */
        case 0x2Bu: return 0x0F; /* Tab */
        case 0x2Cu: return 0x39; /* Space */
        case 0x2Du: return 0x0C; /* - */
        case 0x2Eu: return 0x0D; /* = */
        case 0x2Fu: return 0x1A; /* [ */
        case 0x30u: return 0x1B; /* ] */
        case 0x31u: return 0x2B; /* \ */
        case 0x33u: return 0x27; /* ; */
        case 0x34u: return 0x28; /* ' */
        case 0x35u: return 0x29; /* ` */
        case 0x36u: return 0x33; /* , */
        case 0x37u: return 0x34; /* . */
        case 0x38u: return 0x35; /* / */
        case 0x39u: return 0x3A; /* Caps Lock */
        case 0x44u: return 0x57; /* F11 */
        case 0x45u: return 0x58; /* F12 */
        case 0x49u: *e0 = 1; return 0x52; /* Insert */
        case 0x4Au: *e0 = 1; return 0x47; /* Home */
        case 0x4Bu: *e0 = 1; return 0x49; /* Page Up */
        case 0x4Cu: *e0 = 1; return 0x53; /* Delete */
        case 0x4Du: *e0 = 1; return 0x4F; /* End */
        case 0x4Eu: *e0 = 1; return 0x51; /* Page Down */
        case 0x4Fu: *e0 = 1; return 0x4D; /* Right */
        case 0x50u: *e0 = 1; return 0x4B; /* Left */
        case 0x51u: *e0 = 1; return 0x50; /* Down */
        case 0x52u: *e0 = 1; return 0x48; /* Up */
        case 0x53u: return 0x45; /* Num Lock */
        default: return 0;
    }
}

static void usb_keyboard_emit_set1(unsigned char scancode, int e0, int released) {
    if (scancode == 0) {
        return;
    }
    if (e0) {
        keyboard_inject_scancode(0xE0);
    }
    keyboard_inject_scancode((unsigned char)(scancode | (released ? 0x80u : 0u)));
}

static void usb_keyboard_emit_modifier(unsigned int bit, int released) {
    switch (bit) {
        case 0u: usb_keyboard_emit_set1(0x1D, 0, released); break;
        case 1u: usb_keyboard_emit_set1(0x2A, 0, released); break;
        case 2u: usb_keyboard_emit_set1(0x38, 0, released); break;
        case 4u: usb_keyboard_emit_set1(0x1D, 1, released); break;
        case 5u: usb_keyboard_emit_set1(0x36, 0, released); break;
        case 6u: usb_keyboard_emit_set1(0x38, 1, released); break;
        default: break;
    }
}

static int usb_keyboard_report_has_usage(const u8* report, unsigned int usage) {
    for (unsigned int i = 2u; i < 8u; i++) {
        if (report[i] == usage) {
            return 1;
        }
    }
    return 0;
}

static void usb_keyboard_process_report(void) {
    unsigned int old_mod = s_keyboard_last_report[0];
    unsigned int new_mod = s_keyboard_report_buf[0];

    for (unsigned int bit = 0; bit < 8u; bit++) {
        unsigned int mask = 1u << bit;
        if ((old_mod & mask) != (new_mod & mask)) {
            usb_keyboard_emit_modifier(bit, (new_mod & mask) == 0u);
        }
    }

    for (unsigned int i = 2u; i < 8u; i++) {
        unsigned int usage = s_keyboard_last_report[i];
        int e0;
        unsigned char scancode;
        if (usage == 0u || usb_keyboard_report_has_usage(s_keyboard_report_buf, usage)) {
            continue;
        }
        scancode = usb_hid_usage_to_set1(usage, &e0);
        usb_keyboard_emit_set1(scancode, e0, 1);
    }

    for (unsigned int i = 2u; i < 8u; i++) {
        unsigned int usage = s_keyboard_report_buf[i];
        int e0;
        unsigned char scancode;
        if (usage == 0u || usb_keyboard_report_has_usage(s_keyboard_last_report, usage)) {
            continue;
        }
        scancode = usb_hid_usage_to_set1(usage, &e0);
        usb_keyboard_emit_set1(scancode, e0, 0);
    }

    k_memcpy(s_keyboard_last_report, s_keyboard_report_buf, sizeof(s_keyboard_last_report));
}

static int usb_keyboard_poll_once(void) {
    if (!s_usb_keyboard_active) {
        return 0;
    }

    if (!ohci_td_done(&s_keyboard_intr_td[0])) {
        return 0;
    }

    if (ohci_td_ok(&s_keyboard_intr_td[0])) {
        usb_keyboard_process_report();
        usb_keyboard_queue_intr_td(&s_usb_keyboard_active_dev);
        return 1;
    }

    terminal_puts("usbkbd: intr fail cc=");
    terminal_put_hex((s_keyboard_intr_td[0].control >> OHCI_TD_CC_SHIFT) & 0xFu);
    terminal_putc('\n');
    s_usb_keyboard_active = 0;
    usb_rebuild_periodic_schedule();
    return -1;
}

void usb_mouse_close(void) {
    int old_log;

    if (!s_usb_mouse_active) {
        return;
    }
    if (s_usb_service_active) {
        return;
    }

    old_log = s_usb_mouse_log;
    s_usb_mouse_log = s_usb_mouse_active_log;
    if (s_usb_mouse_active_dev.has_saved_state) {
        ohci_restore_state(s_usb_mouse_active_dev.regs,
                           &s_usb_mouse_active_dev.saved_state);
    }
    s_usb_mouse_log = old_log;
    s_usb_mouse_active = 0;
    s_usb_mouse_active_log = 1;
    s_usb_mouse_last_buttons = 0;
    usb_rebuild_periodic_schedule();
}

int usb_mouse_open_port(unsigned int one_based_port) {
    if (s_usb_mouse_active) {
        return 1;
    }

    if (!usb_find_ohci_mouse(&s_usb_mouse_active_dev, one_based_port)) {
        return 0;
    }

    s_usb_mouse_last_buttons = 0;
    s_usb_mouse_active_log = s_usb_mouse_log;
    s_usb_mouse_active = 1;
    usb_mouse_setup_interrupt(&s_usb_mouse_active_dev);
    usb_mouse_queue_intr_td(&s_usb_mouse_active_dev);
    usb_rebuild_periodic_schedule();
    return 1;
}

int usb_mouse_open_port_quiet(unsigned int one_based_port) {
    int old_log = s_usb_mouse_log;
    int ok;

    s_usb_mouse_log = 0;
    ok = usb_mouse_open_port(one_based_port);
    s_usb_mouse_log = old_log;
    return ok;
}

static int usb_mouse_poll_once_echo(int echo) {
    unsigned int buttons;
    int dx;
    int dy;
    int wheel;

    if (!s_usb_mouse_active) {
        return 0;
    }

    if (!ohci_td_done(&s_intr_td[0])) {
        return 0;
    }

    if (ohci_td_ok(&s_intr_td[0])) {
        buttons = s_report_buf[0] & 0x07u;
        dx = (int)(signed char)s_report_buf[1];
        dy = (int)(signed char)s_report_buf[2];
        wheel = (int)(signed char)s_report_buf[3];

        if (dx || dy || wheel || buttons != s_usb_mouse_last_buttons) {
            mouse_inject_relative(dx, dy, wheel, buttons);
            if (echo) {
                terminal_puts("usbmouse: dx=");
                terminal_put_hex((unsigned int)dx);
                terminal_puts(" dy=");
                terminal_put_hex((unsigned int)dy);
                terminal_puts(" buttons=");
                terminal_put_uint(buttons);
                terminal_putc('\n');
            }
            s_usb_mouse_last_buttons = buttons;
            usb_mouse_queue_intr_td(&s_usb_mouse_active_dev);
            return 1;
        }

        usb_mouse_queue_intr_td(&s_usb_mouse_active_dev);
        return 0;
    }

    if (echo || s_usb_mouse_active_log) {
        terminal_puts("usbmouse: intr fail cc=");
        terminal_put_hex((s_intr_td[0].control >> OHCI_TD_CC_SHIFT) & 0xFu);
        terminal_puts(" ep=");
        terminal_put_uint(s_usb_mouse_active_dev.endpoint);
        terminal_puts(" pkt=");
        terminal_put_uint(s_usb_mouse_active_dev.packet_size);
        terminal_puts(" edh=");
        terminal_put_hex(s_intr_ed.head_td);
        terminal_puts(" edt=");
        terminal_put_hex(s_intr_ed.tail_td);
        terminal_puts(" td=");
        terminal_put_hex(s_intr_td[0].control);
        terminal_putc('\n');
    }
    usb_mouse_close();
    return -1;
}

int usb_mouse_poll_once(void) {
    return usb_mouse_poll_once_echo(0);
}

unsigned int usb_mouse_poll_port(unsigned int seconds, unsigned int one_based_port) {
    unsigned int events = 0;
    unsigned int deadline;

    terminal_puts("usbmouse: command start seconds=");
    terminal_put_uint(seconds);
    terminal_puts(" port=");
    terminal_put_uint(one_based_port);
    terminal_putc('\n');

    if (!usb_mouse_open_port(one_based_port)) {
        terminal_puts("usbmouse: no OHCI boot mouse found\n");
        return 0;
    }

    deadline = timer_get_ticks() + timer_ms_to_ticks_round_up(seconds * 1000u);
    terminal_puts("usbmouse: move/click mouse\n");
    while ((int)(timer_get_ticks() - deadline) < 0) {
        int poll = usb_mouse_poll_once_echo(1);
        if (poll < 0) {
            break;
        }
        if (poll > 0) {
            events++;
        }
    }

    terminal_puts("usbmouse: events=");
    terminal_put_uint(events);
    terminal_putc('\n');
    usb_mouse_close();
    return events;
}

unsigned int usb_mouse_poll(unsigned int seconds) {
    return usb_mouse_poll_port(seconds, 0);
}

static void ohci_power_root_hub_quiet(volatile u32* regs) {
    u32 desc_a = regs[OHCI_HC_RH_DESC_A / 4u];
    u32 ports = desc_a & 0xFFu;

    if (ports > 15u) ports = 15u;
    if (desc_a & OHCI_RHDA_NPS) {
        return;
    }
    if (desc_a & OHCI_RHDA_PSM) {
        for (u32 i = 0; i < ports; i++) {
            regs[(OHCI_HC_RH_PORT_BASE + i * 4u) / 4u] = OHCI_PORT_PPS;
        }
    } else {
        regs[OHCI_HC_RH_STATUS / 4u] = OHCI_RHS_LPSC;
    }
    usb_delay();
}

static void usb_hid_service_enable_periodic(volatile u32* regs) {
    u32 fm_interval = regs[OHCI_HC_FM_INTERVAL / 4u];
    u32 frame_interval = fm_interval & 0x3FFFu;
    if (frame_interval != 0u) {
        regs[OHCI_HC_PERIOD_START / 4u] = (frame_interval * 9u) / 10u;
    }
}

static int usb_hid_service_open(void) {
    int old_log = s_usb_mouse_log;

    if (s_usb_service_active) {
        return 1;
    }

    s_usb_mouse_log = 0;

    for (unsigned int i = 0; i < s_usb_controller_count; i++) {
        usb_controller_t* ctrl = &s_usb_controllers[i];
        volatile u32* regs;
        u32 ports;
        unsigned int next_addr = 1u;

        if (!ctrl->present || ctrl->pci.prog_if != USB_PROG_OHCI || !ctrl->regs) {
            continue;
        }

        regs = ctrl->regs;
        ohci_quiet_interrupts(regs);
        regs[OHCI_HC_CONTROL / 4u] =
            (regs[OHCI_HC_CONTROL / 4u] & ~OHCI_CONTROL_INTERRUPT_ROUTING) |
            OHCI_CONTROL_CLE | OHCI_CONTROL_USB_OPERATIONAL;
        ohci_bind_hcca(regs);
        ohci_power_root_hub_quiet(regs);

        ports = ctrl->ports;
        if (ports == 0u) {
            ports = regs[OHCI_HC_RH_DESC_A / 4u] & 0xFFu;
            if (ports > 15u) ports = 15u;
        }

        for (u32 port = 0; port < ports && next_addr < 8u; port++) {
            usb_mouse_dev_t dev;
            u32 st = regs[(OHCI_HC_RH_PORT_BASE + port * 4u) / 4u];

            if (!(st & OHCI_PORT_CCS)) {
                continue;
            }

            if (!usb_try_hid_on_port(regs, port, &dev, 0u, next_addr)) {
                continue;
            }
            next_addr++;

            dev.has_saved_state = 0;
            if (dev.protocol == USB_HID_PROTOCOL_KEYBOARD && !s_usb_keyboard_active) {
                s_usb_keyboard_active_dev = dev;
                s_usb_keyboard_active = 1;
                k_memset(s_keyboard_last_report, 0, sizeof(s_keyboard_last_report));
                usb_init_interrupt_ed(&s_usb_keyboard_active_dev,
                                      &s_keyboard_intr_ed,
                                      s_keyboard_intr_td);
                usb_keyboard_queue_intr_td(&s_usb_keyboard_active_dev);
                terminal_puts("usb: boot keyboard port=");
                terminal_put_uint(port + 1u);
                terminal_putc('\n');
            } else if (dev.protocol == USB_HID_PROTOCOL_MOUSE && !s_usb_mouse_active) {
                s_usb_mouse_active_dev = dev;
                s_usb_mouse_active = 1;
                s_usb_mouse_active_log = 0;
                s_usb_mouse_last_buttons = 0;
                usb_init_interrupt_ed(&s_usb_mouse_active_dev, &s_intr_ed, s_intr_td);
                usb_mouse_queue_intr_td(&s_usb_mouse_active_dev);
                terminal_puts("usb: boot mouse port=");
                terminal_put_uint(port + 1u);
                terminal_putc('\n');
            }
        }

        if (s_usb_keyboard_active || s_usb_mouse_active) {
            usb_hid_service_enable_periodic(regs);
            s_usb_service_active = 1;
            usb_rebuild_periodic_schedule();
            s_usb_mouse_log = old_log;
            return 1;
        }
    }

    s_usb_mouse_log = old_log;
    return 0;
}

static void usb_service_main(void) {
    for (;;) {
        if (!s_usb_service_active && !s_usb_service_probe_done) {
            (void)usb_hid_service_open();
            s_usb_service_probe_done = 1;
        }
        if (s_usb_service_active) {
            (void)usb_keyboard_poll_once();
            (void)usb_mouse_poll_once();
        }
        __asm__ __volatile__("sti; hlt");
    }
}

int usb_start_service(void) {
    process_t* proc;

    if (s_usb_service_started) {
        return 1;
    }

    proc = process_create_kernel_task("usb", usb_service_main);
    if (!proc) {
        return 0;
    }
    if (!sched_enqueue(proc)) {
        process_destroy(proc);
        return 0;
    }

    s_usb_service_started = 1;
    return 1;
}

void usb_debug_snapshot(usb_debug_state_t* out) {
    if (!out) return;
    *out = s_usb_debug;
}
