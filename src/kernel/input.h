#ifndef INPUT_H
#define INPUT_H

#include "uapi_input.h"
#include "../drivers/keyboard.h"

void input_init(void);
void input_clear_events(void);

void input_push_key_event(key_event_t ev);
void input_push_mouse_event(int dx, int dy,
                            unsigned int buttons,
                            unsigned int button_changes);

int input_available(void);
int input_pop_event(sys_input_event_t* out);

void input_set_waiting_process(void* proc);
void* input_get_waiting_process(void);
void input_forget_waiting_process(void* proc);

#endif /* INPUT_H */
