#include "memory.h"

#define PAGE_SIZE 4096u

static unsigned int heap_current = 0;

void memory_init(unsigned int start) {
    heap_current = start;
}

void* kmalloc(unsigned int size) {
    if (size == 0) {
        return 0;
    }

    void* addr = (void*)heap_current;
    heap_current += size;

    /* align to 4 bytes */
    if (heap_current & 0x3) {
        heap_current = (heap_current & ~0x3) + 4;
    }

    return addr;
}

/*
 * kmalloc_page()
 *
 * Allocate one page-aligned 4096-byte block from the bump allocator.
 * Used for kernel-owned structures (process page directories, parse
 * buffers) that do not need to be freed.
 *
 * User-space page frames (ELF segments, user stack, user page tables)
 * are allocated from the PMM instead so they can be reclaimed on exit.
 */
void* kmalloc_page(void) {
    /* round up to next page boundary */
    if (heap_current & (PAGE_SIZE - 1)) {
        heap_current = (heap_current + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    }

    void* addr = (void*)heap_current;
    heap_current += PAGE_SIZE;
    return addr;
}

/*
 * memory_get_heap_top()
 *
 * Returns the current bump pointer.  Used by:
 *   - pmm_init() to determine which frames are already consumed
 *   - the meminfo shell command for reporting heap usage
 */
unsigned int memory_get_heap_top(void) {
    return heap_current;
}