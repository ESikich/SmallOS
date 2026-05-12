#ifndef MOUSE_H
#define MOUSE_H

#include "uapi_input.h"

int mouse_init(void);
void mouse_handle_irq(void);
int mouse_read_state(sys_mouse_state_t* out);
int mouse_available(void);

#endif /* MOUSE_H */
