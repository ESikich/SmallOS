#ifndef SMALLOS_GUI_WINDOW_H
#define SMALLOS_GUI_WINDOW_H

#define GUI_WINDOW_CAPACITY 8

typedef struct {
    int order[GUI_WINDOW_CAPACITY];
    int count;
} gui_window_stack_t;

void gui_window_stack_init(gui_window_stack_t* stack);
void gui_window_stack_remove(gui_window_stack_t* stack, int window_id);
void gui_window_stack_raise(gui_window_stack_t* stack, int window_id);
int gui_window_stack_top(const gui_window_stack_t* stack);
int gui_window_stack_count(const gui_window_stack_t* stack);
int gui_window_stack_at(const gui_window_stack_t* stack, int position);

#endif
