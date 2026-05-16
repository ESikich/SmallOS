#ifndef USB_H
#define USB_H

typedef struct {
    unsigned int controller_count;
    unsigned int uhci_count;
    unsigned int ohci_count;
    unsigned int ehci_count;
    unsigned int xhci_count;
    unsigned int powered_port_count;
    unsigned int last_bar;
    unsigned int last_ports;
    unsigned int last_port_status0;
    unsigned int last_port_status1;
    unsigned char last_bus;
    unsigned char last_slot;
    unsigned char last_func;
    unsigned char last_prog_if;
} usb_debug_state_t;

typedef struct {
    void* controller;
    volatile unsigned int* regs;
    unsigned int address;
    unsigned int low_speed;
    unsigned int interface_number;
    unsigned int bulk_in_endpoint;
    unsigned int bulk_out_endpoint;
    unsigned int bulk_in_packet_size;
    unsigned int bulk_out_packet_size;
    unsigned int bulk_in_toggle;
    unsigned int bulk_out_toggle;
    unsigned int controller_index;
    unsigned int port_index;
} usb_mass_device_t;

void usb_init(void);
int usb_start_service(void);
int usb_probe_hid(void);
void usb_diag(void);
void usb_dump_ports(void);
void usb_peek_port(unsigned int one_based_port);
unsigned int usb_power_ohci_ports(void);
int usb_mouse_open_port(unsigned int one_based_port);
int usb_mouse_open_port_quiet(unsigned int one_based_port);
int usb_mouse_poll_once(void);
void usb_mouse_close(void);
unsigned int usb_mouse_poll(unsigned int seconds);
unsigned int usb_mouse_poll_port(unsigned int seconds, unsigned int one_based_port);
void usb_debug_snapshot(usb_debug_state_t* out);
int usb_find_mass_storage(usb_mass_device_t* out);
int usb_bulk_in(usb_mass_device_t* dev, void* data, unsigned int len, unsigned int* out_len);
int usb_bulk_out(usb_mass_device_t* dev, const void* data, unsigned int len);

#endif /* USB_H */
