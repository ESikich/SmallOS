#ifndef KALLOC_H
#define KALLOC_H

typedef struct kalloc_stats {
    unsigned int pages;
    unsigned int used_bytes;
    unsigned int free_bytes;
    unsigned int invalid_frees;
    unsigned int double_frees;
} kalloc_stats_t;

void* kalloc(unsigned int size);
void* kcalloc(unsigned int count, unsigned int size);
void  kfree(void* ptr);
unsigned int kalloc_stats(kalloc_stats_t* out);

#endif /* KALLOC_H */
