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
    unsigned int ready;
    unsigned int init_step;
    unsigned int init_fail;
    unsigned int config_before;
    unsigned int config_after;
} mouse_debug_state_t;

int mouse_init(void);
void mouse_handle_irq(void);
void mouse_enable_external_source(void);
void mouse_inject_relative(int dx, int dy, int wheel, unsigned int buttons);
int mouse_read_state(sys_mouse_state_t* out);
int mouse_available(void);
void mouse_debug_snapshot(mouse_debug_state_t* out);

#endif /* MOUSE_H */
