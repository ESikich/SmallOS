#ifndef MEMORY_H
#define MEMORY_H

void          memory_init(unsigned int start);
void*         kmalloc(unsigned int size);
void*         kmalloc_page(void);           /* allocate one 4096-byte page-aligned block */
unsigned int  memory_get_heap_top(void);    /* current bump pointer (for PMM + meminfo)  */

#endif