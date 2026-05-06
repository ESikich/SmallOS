#ifndef WAIT_H
#define WAIT_H

typedef struct process process_t;
typedef struct wait_node wait_node_t;

typedef struct wait_queue {
    wait_node_t* head;
} wait_queue_t;

void wait_queue_init(wait_queue_t* queue);
int  wait_queue_add(wait_queue_t* queue, process_t* proc);
void wait_queue_remove_proc(process_t* proc);
void wait_queue_wake_one(wait_queue_t* queue);
void wait_queue_wake_all(wait_queue_t* queue);

#endif /* WAIT_H */
