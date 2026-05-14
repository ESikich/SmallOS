#ifndef MOUSE_H
#define MOUSE_H

#include "uapi_input.h"

typedef struct mouse_debug_state {
    unsigned int irq_count;
    unsigned int byte_count;
    unsigned int aux_status_count;
    unsigned int packet_count;
    unsigned int vmware_packet_count;
    unsigned int sync_drop_count;
    unsigned int overflow_drop_count;
    unsigned int vmware_enabled;
    unsigned int packet_size;
    unsigned int device_id;
} mouse_debug_state_t;

int mouse_init(void);
void mouse_handle_irq(void);
int mouse_read_state(sys_mouse_state_t* out);
int mouse_available(void);
void mouse_debug_snapshot(mouse_debug_state_t* out);

#endif /* MOUSE_H */
