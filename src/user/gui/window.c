#include "window.h"

void gui_window_stack_init(gui_window_stack_t* stack) {
    if (!stack) return;
    stack->count = 0;
}

void gui_window_stack_remove(gui_window_stack_t* stack, int window_id) {
    int out = 0;
    if (!stack) return;
    for (int i = 0; i < stack->count; i++) {
        if (stack->order[i] != window_id) stack->order[out++] = stack->order[i];
    }
    stack->count = out;
}

void gui_window_stack_raise(gui_window_stack_t* stack, int window_id) {
    if (!stack) return;
    gui_window_stack_remove(stack, window_id);
    if (stack->count < GUI_WINDOW_CAPACITY)
        stack->order[stack->count++] = window_id;
}

int gui_window_stack_top(const gui_window_stack_t* stack) {
    if (!stack || stack->count == 0) return -1;
    return stack->order[stack->count - 1];
}

int gui_window_stack_count(const gui_window_stack_t* stack) {
    return stack ? stack->count : 0;
}

int gui_window_stack_at(const gui_window_stack_t* stack, int position) {
    if (!stack || position < 0 || position >= stack->count) return -1;
    return stack->order[position];
}
