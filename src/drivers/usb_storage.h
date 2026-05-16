#ifndef USB_STORAGE_H
#define USB_STORAGE_H

#include "block.h"

int usb_storage_init(void);
block_device_t* usb_storage_block_device(void);

#endif /* USB_STORAGE_H */
