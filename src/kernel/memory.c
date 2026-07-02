#include "memory.h"
#include "pmm.h"

#define PAGE_SIZE 4096u

static unsigned int heap_current = 0;
static unsigned int heap_base = 0;

void memory_init(unsigned int start) {
    heap_base = start;
    heap_current = start;
}

void* kmalloc(unsigned int size) {
    unsigned int old_top;

    if (size == 0) {
        return 0;
    }

    old_top = heap_current;
    void* addr = (void*)old_top;
    heap_current += size;

    /* align to 4 bytes */
    if (heap_current & 0x3) {
        heap_current = (heap_current & ~0x3) + 4;
    }

    pmm_reserve_range(old_top, heap_current);
    return addr;
}

/*
 * kmalloc_page()
 *
 * Allocate one page-aligned 4096-byte block from the bump allocator.
 * Used for kernel-owned, long-lived structures that do not need to be
 * freed (for example, kernel tables or bookkeeping buffers).
 * Once the PMM is online, each bump allocation reserves the physical pages it
 * spans so the frame allocator cannot reuse kernel-owned memory.
 *
 * Reclaimable page frames used for user-space mappings are allocated
 * from the PMM instead so they can be freed later.
 */
void* kmalloc_page(void) {
    unsigned int old_top;

    /* round up to next page boundary */
    if (heap_current & (PAGE_SIZE - 1)) {
        heap_current = (heap_current + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
    }

    old_top = heap_current;
    void* addr = (void*)old_top;
    heap_current += PAGE_SIZE;
    pmm_reserve_range(old_top, heap_current);
    return addr;
}

/*
 * memory_get_heap_top()
 *
 * Returns the current bump pointer.
 * Used by the meminfo shell command for reporting heap usage.
 */
unsigned int memory_get_heap_top(void) {
    return heap_current;
}

unsigned int memory_get_heap_base(void) {
    return heap_base;
}
