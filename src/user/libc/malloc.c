#include "user_lib.h"

typedef struct heap_block {
    unsigned int size;
    unsigned int free;
    struct heap_block* next;
    struct heap_block* prev;
} heap_block_t;

static heap_block_t* s_heap_head = 0;
static unsigned int s_heap_brk = 0;

static unsigned int align8(unsigned int n) {
    return (n + 7u) & ~7u;
}

static unsigned int align_page(unsigned int n) {
    return (n + 4095u) & ~4095u;
}

static unsigned char* block_payload(heap_block_t* block) {
    return (unsigned char*)block + sizeof(heap_block_t);
}

static unsigned char* block_end(heap_block_t* block) {
    return block_payload(block) + block->size;
}

static heap_block_t* heap_last_block(void) {
    heap_block_t* block = s_heap_head;
    if (!block) return 0;
    while (block->next) {
        block = block->next;
    }
    return block;
}

static void heap_init(void) {
    if (s_heap_brk != 0) {
        return;
    }

    s_heap_brk = sys_brk(0);
    if (s_heap_brk < USER_HEAP_BASE) {
        s_heap_brk = USER_HEAP_BASE;
    }
}

static heap_block_t* heap_extend(unsigned int min_payload) {
    unsigned int old_end;
    unsigned int grow_bytes;
    unsigned int new_end;
    heap_block_t* last;
    heap_block_t* block;

    heap_init();

    old_end = s_heap_brk;
    grow_bytes = align_page(sizeof(heap_block_t) + min_payload);
    if (grow_bytes < 4096u) {
        grow_bytes = 4096u;
    }

    new_end = old_end + grow_bytes;
    if (new_end < old_end) {
        return 0;
    }

    if (sys_brk(new_end) != new_end) {
        return 0;
    }

    s_heap_brk = new_end;

    last = heap_last_block();
    if (last && last->free && block_end(last) == (unsigned char*)old_end) {
        last->size += grow_bytes;
        return last;
    }

    block = (heap_block_t*)old_end;
    block->size = grow_bytes - sizeof(heap_block_t);
    block->free = 1;
    block->next = 0;
    block->prev = last;

    if (last) {
        last->next = block;
    } else {
        s_heap_head = block;
    }

    return block;
}

static heap_block_t* heap_find_fit(unsigned int size) {
    heap_block_t* block = s_heap_head;
    while (block) {
        if (block->free && block->size >= size) {
            return block;
        }
        block = block->next;
    }
    return 0;
}

static void heap_split_block(heap_block_t* block, unsigned int size) {
    unsigned int remaining;
    heap_block_t* next;

    if (block->size <= size + sizeof(heap_block_t) + 8u) {
        return;
    }

    remaining = block->size - size - sizeof(heap_block_t);
    next = (heap_block_t*)(block_payload(block) + size);
    next->size = remaining;
    next->free = 1;
    next->next = block->next;
    next->prev = block;

    if (next->next) {
        next->next->prev = next;
    }

    block->next = next;
    block->size = size;
}

static void heap_coalesce(heap_block_t* block) {
    if (!block) {
        return;
    }

    if (block->prev && block->prev->free && block_end(block->prev) == (unsigned char*)block) {
        heap_block_t* prev = block->prev;
        prev->size += sizeof(heap_block_t) + block->size;
        prev->next = block->next;
        if (block->next) {
            block->next->prev = prev;
        }
        block = prev;
    }

    while (block->next && block->next->free && block_end(block) == (unsigned char*)block->next) {
        heap_block_t* next = block->next;
        block->size += sizeof(heap_block_t) + next->size;
        block->next = next->next;
        if (block->next) {
            block->next->prev = block;
        }
    }
}

void* malloc(size_t size) {
    heap_block_t* block;
    unsigned int need;

    if (size == 0) {
        return 0;
    }

    need = align8((unsigned int)size);
    block = heap_find_fit(need);
    if (!block) {
        if (!heap_extend(need)) {
            return 0;
        }
        block = heap_find_fit(need);
        if (!block) {
            return 0;
        }
    }

    heap_split_block(block, need);
    block->free = 0;
    return block_payload(block);
}

void free(void* ptr) {
    heap_block_t* block;

    if (!ptr) {
        return;
    }

    heap_init();
    if ((unsigned int)ptr < USER_HEAP_BASE || (unsigned int)ptr >= s_heap_brk) {
        return;
    }

    block = (heap_block_t*)((unsigned char*)ptr - sizeof(heap_block_t));
    block->free = 1;
    heap_coalesce(block);
}

void* realloc(void* ptr, size_t size) {
    void* new_ptr;
    heap_block_t* block;
    unsigned int need;

    if (!ptr) {
        return malloc(size);
    }

    if (size == 0) {
        free(ptr);
        return 0;
    }

    heap_init();
    if ((unsigned int)ptr < USER_HEAP_BASE || (unsigned int)ptr >= s_heap_brk) {
        return 0;
    }

    need = align8((unsigned int)size);
    block = (heap_block_t*)((unsigned char*)ptr - sizeof(heap_block_t));

    if (block->size >= need) {
        heap_split_block(block, need);
        return ptr;
    }

    new_ptr = malloc(size);
    if (!new_ptr) {
        return 0;
    }

    memcpy(new_ptr, ptr, block->size);
    free(ptr);
    return new_ptr;
}

void* calloc(size_t nmemb, size_t size) {
    unsigned int total;
    void* ptr;

    if (nmemb == 0 || size == 0) {
        return malloc(1);
    }

    if (nmemb > 0xFFFFFFFFu / size) {
        return 0;
    }

    total = (unsigned int)(nmemb * size);
    ptr = malloc(total);
    if (!ptr) {
        return 0;
    }

    memset(ptr, 0, total);
    return ptr;
}
