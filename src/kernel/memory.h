#ifndef MEMORY_H
#define MEMORY_H

/*
 * Top of the boot stack, established by loader2's generated stack constants.
 * This is also the fallback ESP0 for kernel tasks that have no per-process
 * kernel stack frame.
 */
#define KERNEL_BOOT_STACK_TOP 0xF0000u

void          memory_init(unsigned int start);
void*         kmalloc(unsigned int size);
void*         kmalloc_page(void);           /* allocate one 4096-byte page-aligned block */
unsigned int  memory_get_heap_top(void);    /* current bump pointer (for meminfo/debug)   */

#endif
