#ifndef SMALLOS_GUI_TASKS_MODEL_H
#define SMALLOS_GUI_TASKS_MODEL_H

typedef struct gui_task_model_row {
    unsigned int pid;
    unsigned int parent_pid;
    unsigned int state;
    unsigned int cpu_delta;
    unsigned int ram_bytes;
    unsigned int heap_bytes;
    const char* name;
} gui_task_model_row_t;

int gui_tasks_compare(const gui_task_model_row_t* a,
                      const gui_task_model_row_t* b,
                      int column);
void gui_tasks_sort(gui_task_model_row_t* rows, unsigned int count,
                    int column, int descending);
int gui_tasks_is_protected(const gui_task_model_row_t* row,
                           unsigned int gui_pid);

#endif
