#include "wait.h"

#include "process.h"
#include "uapi_errno.h"

#define WAIT_NODE_MAX 128u

struct wait_node {
    wait_queue_t* queue;
    process_t* proc;
    wait_node_t* next;
};

static wait_node_t s_wait_nodes[WAIT_NODE_MAX];

void wait_queue_init(wait_queue_t* queue) {
    if (!queue) return;
    queue->head = 0;
}

static wait_node_t* wait_queue_find(wait_queue_t* queue, process_t* proc) {
    if (!queue || !proc) return 0;

    wait_node_t* node = queue->head;
    while (node) {
        if (node->proc == proc) {
            return node;
        }
        node = node->next;
    }
    return 0;
}

static void wait_queue_remove_node(wait_node_t* node) {
    wait_queue_t* queue;
    wait_node_t** cursor;

    if (!node) return;
    queue = node->queue;
    if (queue) {
        cursor = &queue->head;
        while (*cursor) {
            if (*cursor == node) {
                *cursor = node->next;
                break;
            }
            cursor = &(*cursor)->next;
        }
    }

    node->queue = 0;
    node->proc = 0;
    node->next = 0;
}

int wait_queue_add(wait_queue_t* queue, process_t* proc) {
    if (!queue || !proc) return -EINVAL;
    if (wait_queue_find(queue, proc)) return 0;

    for (unsigned int i = 0; i < WAIT_NODE_MAX; i++) {
        wait_node_t* node = &s_wait_nodes[i];
        if (!node->proc) {
            node->queue = queue;
            node->proc = proc;
            node->next = queue->head;
            queue->head = node;
            return 0;
        }
    }

    return -ENOMEM;
}

void wait_queue_remove_proc(process_t* proc) {
    if (!proc) return;

    for (unsigned int i = 0; i < WAIT_NODE_MAX; i++) {
        if (s_wait_nodes[i].proc == proc) {
            wait_queue_remove_node(&s_wait_nodes[i]);
        }
    }
}

static void wait_queue_wake_proc(process_t* proc) {
    if (!proc) return;

    if (proc->state == PROCESS_STATE_WAITING ||
        proc->state == PROCESS_STATE_SLEEPING) {
        proc->state = PROCESS_STATE_RUNNING;
        proc->sleep_until = 0u;
    }
}

void wait_queue_wake_one(wait_queue_t* queue) {
    process_t* proc;

    if (!queue || !queue->head) return;

    proc = queue->head->proc;
    wait_queue_wake_proc(proc);
    if (proc) {
        wait_queue_remove_proc(proc);
    } else {
        wait_queue_remove_node(queue->head);
    }
}

void wait_queue_wake_all(wait_queue_t* queue) {
    if (!queue) return;

    while (queue->head) {
        process_t* proc = queue->head->proc;
        wait_queue_wake_proc(proc);
        if (proc) {
            wait_queue_remove_proc(proc);
        } else {
            wait_queue_remove_node(queue->head);
        }
    }
}
