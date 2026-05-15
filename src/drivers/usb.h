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

void usb_init(void);
int usb_start_service(void);
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

#endif /* USB_H */
