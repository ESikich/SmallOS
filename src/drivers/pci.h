#ifndef PCI_H
#define PCI_H

typedef struct {
    unsigned char bus;
    unsigned char slot;
    unsigned char func;
    unsigned short vendor_id;
    unsigned short device_id;
    unsigned char class_code;
    unsigned char subclass;
    unsigned char prog_if;
    unsigned char revision_id;
    unsigned char header_type;
} pci_device_t;

void pci_init(void);

unsigned int pci_read_config_dword(unsigned char bus,
                                   unsigned char slot,
                                   unsigned char func,
                                   unsigned char offset);
unsigned short pci_read_config_word(unsigned char bus,
                                    unsigned char slot,
                                    unsigned char func,
                                    unsigned char offset);
unsigned char pci_read_config_byte(unsigned char bus,
                                   unsigned char slot,
                                   unsigned char func,
                                   unsigned char offset);
void pci_read_device(unsigned char bus,
                     unsigned char slot,
                     unsigned char func,
                     pci_device_t* out);

#endif /* PCI_H */
