#include "pci.h"

#include "ports.h"
#include "terminal.h"

#define PCI_CONFIG_ADDRESS 0xCF8
#define PCI_CONFIG_DATA    0xCFC

#define PCI_CLASS_NETWORK 0x02

static void pci_put_byte_hex(unsigned char value) {
    static const char hex[] = "0123456789ABCDEF";
    terminal_putc(hex[(value >> 4) & 0xF]);
    terminal_putc(hex[value & 0xF]);
}

static unsigned int pci_config_address(unsigned char bus,
                                       unsigned char slot,
                                       unsigned char func,
                                       unsigned char offset) {
    /* Standard PCI config mechanism #1 address format. */
    return 0x80000000u
         | ((unsigned int)bus << 16)
         | ((unsigned int)slot << 11)
         | ((unsigned int)func << 8)
         | (unsigned int)(offset & 0xFC);
}

unsigned int pci_read_config_dword(unsigned char bus,
                                   unsigned char slot,
                                   unsigned char func,
                                   unsigned char offset) {
    unsigned int address = pci_config_address(bus, slot, func, offset);

    outl(PCI_CONFIG_ADDRESS, address);
    return inl(PCI_CONFIG_DATA);
}

unsigned short pci_read_config_word(unsigned char bus,
                                    unsigned char slot,
                                    unsigned char func,
                                    unsigned char offset) {
    unsigned int value = pci_read_config_dword(bus, slot, func, offset);
    return (unsigned short)((value >> ((offset & 2) * 8)) & 0xFFFFu);
}

unsigned char pci_read_config_byte(unsigned char bus,
                                   unsigned char slot,
                                   unsigned char func,
                                   unsigned char offset) {
    unsigned int value = pci_read_config_dword(bus, slot, func, offset);
    return (unsigned char)((value >> ((offset & 3) * 8)) & 0xFFu);
}

void pci_write_config_dword(unsigned char bus,
                            unsigned char slot,
                            unsigned char func,
                            unsigned char offset,
                            unsigned int value) {
    outl(PCI_CONFIG_ADDRESS, pci_config_address(bus, slot, func, offset));
    outl(PCI_CONFIG_DATA, value);
}

void pci_write_config_word(unsigned char bus,
                           unsigned char slot,
                           unsigned char func,
                           unsigned char offset,
                           unsigned short value) {
    unsigned int addr = pci_config_address(bus, slot, func, offset);
    unsigned int shift = (offset & 2u) * 8u;
    unsigned int current;

    outl(PCI_CONFIG_ADDRESS, addr);
    current = inl(PCI_CONFIG_DATA);
    current &= ~(0xFFFFu << shift);
    current |= ((unsigned int)value << shift);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, current);
}

void pci_write_config_byte(unsigned char bus,
                           unsigned char slot,
                           unsigned char func,
                           unsigned char offset,
                           unsigned char value) {
    unsigned int addr = pci_config_address(bus, slot, func, offset);
    unsigned int shift = (offset & 3u) * 8u;
    unsigned int current;

    outl(PCI_CONFIG_ADDRESS, addr);
    current = inl(PCI_CONFIG_DATA);
    current &= ~(0xFFu << shift);
    current |= ((unsigned int)value << shift);
    outl(PCI_CONFIG_ADDRESS, addr);
    outl(PCI_CONFIG_DATA, current);
}

void pci_read_device(unsigned char bus,
                     unsigned char slot,
                     unsigned char func,
                     pci_device_t* out) {
    unsigned int id = pci_read_config_dword(bus, slot, func, 0x00);
    unsigned int class_reg = pci_read_config_dword(bus, slot, func, 0x08);

    out->bus = bus;
    out->slot = slot;
    out->func = func;
    out->vendor_id = (unsigned short)(id & 0xFFFFu);
    out->device_id = (unsigned short)((id >> 16) & 0xFFFFu);
    out->revision_id = (unsigned char)(class_reg & 0xFFu);
    out->prog_if = (unsigned char)((class_reg >> 8) & 0xFFu);
    out->subclass = (unsigned char)((class_reg >> 16) & 0xFFu);
    out->class_code = (unsigned char)((class_reg >> 24) & 0xFFu);
    out->header_type = pci_read_config_byte(bus, slot, func, 0x0E);
}

void pci_init(void) {
    unsigned int device_count = 0;
    unsigned int network_count = 0;

    terminal_puts("pci: scan\n");

    /* Walk the conventional PCI bus space and report anything present. */
    for (unsigned int bus = 0; bus < 256; bus++) {
        for (unsigned int slot = 0; slot < 32; slot++) {
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

                if (dev.vendor_id == 0xFFFFu) {
                    continue;
                }

                device_count++;

                if (dev.class_code == PCI_CLASS_NETWORK) {
                    network_count++;
                    /* Log network-class devices so NIC bring-up is visible. */
                    terminal_puts("pci: network ");
                    pci_put_byte_hex(dev.bus);
                    terminal_putc(':');
                    pci_put_byte_hex(dev.slot);
                    terminal_putc('.');
                    terminal_putc((char)('0' + dev.func));
                    terminal_puts(" vendor=");
                    terminal_put_hex(dev.vendor_id);
                    terminal_puts(" device=");
                    terminal_put_hex(dev.device_id);
                    terminal_puts(" class=");
                    pci_put_byte_hex(dev.class_code);
                    terminal_putc('/');
                    pci_put_byte_hex(dev.subclass);
                    terminal_putc('\n');
                }
            }
        }
    }

    terminal_puts("pci: devices=");
    terminal_put_uint(device_count);
    terminal_puts(" network=");
    terminal_put_uint(network_count);
    terminal_putc('\n');

    if (network_count == 0) {
        terminal_puts("pci: no network controller detected\n");
    }
}

int pci_find_device(unsigned short vendor_id,
                    unsigned short device_id,
                    pci_device_t* out) {
    /* Helper for specific drivers that know their vendor/device IDs. */
    for (unsigned int bus = 0; bus < 256; bus++) {
        for (unsigned int slot = 0; slot < 32; slot++) {
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
                unsigned short found_vendor = pci_read_config_word((unsigned char)bus,
                                                                   (unsigned char)slot,
                                                                   (unsigned char)func,
                                                                   0x00);
                if (found_vendor != vendor_id) {
                    continue;
                }

                unsigned short found_device = pci_read_config_word((unsigned char)bus,
                                                                   (unsigned char)slot,
                                                                   (unsigned char)func,
                                                                   0x02);
                if (found_device != device_id) {
                    continue;
                }

                if (out) {
                    pci_read_device((unsigned char)bus,
                                    (unsigned char)slot,
                                    (unsigned char)func,
                                    out);
                }
                return 1;
            }
        }
    }

    return 0;
}
