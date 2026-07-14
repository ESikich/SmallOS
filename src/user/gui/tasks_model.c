#include "tasks_model.h"

static int text_compare(const char* a, const char* b) {
    while (a && b && *a && *b) {
        unsigned char ca = (unsigned char)*a++;
        unsigned char cb = (unsigned char)*b++;
        if (ca >= 'A' && ca <= 'Z') ca = (unsigned char)(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = (unsigned char)(cb + 32);
        if (ca != cb) return ca < cb ? -1 : 1;
    }
    return a && *a ? 1 : b && *b ? -1 : 0;
}

int gui_tasks_compare(const gui_task_model_row_t* a,
                      const gui_task_model_row_t* b,
                      int column) {
    unsigned int av;
    unsigned int bv;
    if (column == 1) return text_compare(a->name, b->name);
    if (column == 0) { av = a->pid; bv = b->pid; }
    else if (column == 2) { av = a->state; bv = b->state; }
    else if (column == 3) { av = a->cpu_delta; bv = b->cpu_delta; }
    else if (column == 4) { av = a->ram_bytes; bv = b->ram_bytes; }
    else { av = a->heap_bytes; bv = b->heap_bytes; }
    return av < bv ? -1 : av > bv ? 1 : 0;
}

void gui_tasks_sort(gui_task_model_row_t* rows, unsigned int count,
                    int column, int descending) {
    for (unsigned int i = 0; i < count; i++) {
        for (unsigned int j = i + 1; j < count; j++) {
            int comparison = gui_tasks_compare(&rows[j], &rows[i], column);
            if ((comparison > 0) == !!descending && comparison != 0) {
                gui_task_model_row_t swap = rows[i];
                rows[i] = rows[j];
                rows[j] = swap;
            }
        }
    }
}

int gui_tasks_is_protected(const gui_task_model_row_t* row,
                           unsigned int gui_pid) {
    return !row || row->pid == 1u || row->pid == gui_pid ||
           row->parent_pid == 0u;
}
