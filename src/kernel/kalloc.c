#include "kalloc.h"
#include "pmm.h"
#include "paging.h"
#include "klib.h"
#include "terminal.h"
#include "uapi_errno.h"

#define KALLOC_SLAB_MAGIC 0x4B534C42u /* KSLB */
#define KALLOC_LARGE_MAGIC 0x4B4C4152u /* KLAR */
#define KALLOC_FREED_MAGIC 0x4B465245u /* KFRE */
#define KALLOC_SMALL_MAX 1024u
#define KALLOC_CLASS_COUNT 6u

typedef struct kalloc_slab_page {
    unsigned int magic;
    unsigned int class_index;
    unsigned int block_size;
    unsigned int total_blocks;
    unsigned int free_blocks;
    void* free_list;
    struct kalloc_slab_page* next;
} kalloc_slab_page_t;

typedef struct kalloc_large_header {
    unsigned int magic;
    unsigned int frames;
    unsigned int size;
} kalloc_large_header_t;

static const unsigned int s_class_sizes[KALLOC_CLASS_COUNT] = {
    16u, 32u, 64u, 128u, 256u, 1024u
};

static kalloc_slab_page_t* s_slabs[KALLOC_CLASS_COUNT];
static kalloc_stats_t s_stats;

static unsigned int align_up(unsigned int value, unsigned int align) {
    return (value + align - 1u) & ~(align - 1u);
}

static int class_for_size(unsigned int size) {
    for (unsigned int i = 0; i < KALLOC_CLASS_COUNT; i++) {
        if (size <= s_class_sizes[i]) return (int)i;
    }
    return -1;
}

static kalloc_slab_page_t* slab_create(unsigned int class_index) {
    u32 frame = pmm_alloc_frame();
    if (!frame) return 0;

    kalloc_slab_page_t* slab = (kalloc_slab_page_t*)paging_phys_to_kernel_virt(frame);
    unsigned int block_size = s_class_sizes[class_index];
    unsigned int start = align_up(sizeof(kalloc_slab_page_t), sizeof(void*));
    unsigned int total = (PAGE_SIZE - start) / block_size;
    unsigned char* base = (unsigned char*)slab;

    k_memset(slab, 0, PAGE_SIZE);
    slab->magic = KALLOC_SLAB_MAGIC;
    slab->class_index = class_index;
    slab->block_size = block_size;
    slab->total_blocks = total;
    slab->free_blocks = total;
    slab->next = s_slabs[class_index];
    s_slabs[class_index] = slab;

    slab->free_list = 0;
    for (unsigned int i = 0; i < total; i++) {
        void* slot = base + start + i * block_size;
        *(void**)slot = slab->free_list;
        slab->free_list = slot;
    }

    s_stats.pages++;
    s_stats.free_bytes += total * block_size;
    return slab;
}

static void slab_remove_if_empty(kalloc_slab_page_t* slab) {
    unsigned int class_index;
    kalloc_slab_page_t** cur;

    if (!slab || slab->magic != KALLOC_SLAB_MAGIC) return;
    if (slab->free_blocks != slab->total_blocks) return;

    class_index = slab->class_index;
    if (class_index >= KALLOC_CLASS_COUNT) return;

    cur = &s_slabs[class_index];
    while (*cur) {
        if (*cur == slab) {
            *cur = slab->next;
            s_stats.pages--;
            s_stats.free_bytes -= slab->total_blocks * slab->block_size;
            slab->magic = KALLOC_FREED_MAGIC;
            pmm_free_frame(paging_kernel_virt_to_phys(slab));
            return;
        }
        cur = &(*cur)->next;
    }
}

void* kalloc(unsigned int size) {
    int class_index;

    if (size == 0u) return 0;

    class_index = class_for_size(size);
    if (class_index >= 0) {
        kalloc_slab_page_t* slab = s_slabs[class_index];
        while (slab && slab->free_blocks == 0u) {
            slab = slab->next;
        }
        if (!slab) {
            slab = slab_create((unsigned int)class_index);
            if (!slab) return 0;
        }

        void* slot = slab->free_list;
        slab->free_list = *(void**)slot;
        slab->free_blocks--;
        s_stats.free_bytes -= slab->block_size;
        s_stats.used_bytes += slab->block_size;
        k_memset(slot, 0, slab->block_size);
        return slot;
    }

    unsigned int total = size + sizeof(kalloc_large_header_t);
    unsigned int frames = PAGE_ALIGN(total) / PAGE_SIZE;
    u32 frame = pmm_alloc_contiguous_frames(frames);
    if (!frame) return 0;

    kalloc_large_header_t* hdr = (kalloc_large_header_t*)paging_phys_to_kernel_virt(frame);
    hdr->magic = KALLOC_LARGE_MAGIC;
    hdr->frames = frames;
    hdr->size = size;
    s_stats.pages += frames;
    s_stats.used_bytes += size;
    k_memset((unsigned char*)hdr + sizeof(*hdr), 0, frames * PAGE_SIZE - sizeof(*hdr));
    return (unsigned char*)hdr + sizeof(*hdr);
}

void* kcalloc(unsigned int count, unsigned int size) {
    if (count == 0u || size == 0u) return 0;
    if (count > 0xFFFFFFFFu / size) return 0;
    return kalloc(count * size);
}

void kfree(void* ptr) {
    if (!ptr) return;

    kalloc_large_header_t* large = (kalloc_large_header_t*)((unsigned char*)ptr - sizeof(kalloc_large_header_t));
    if (large->magic == KALLOC_LARGE_MAGIC) {
        unsigned int frames = large->frames;
        unsigned int size = large->size;
        large->magic = KALLOC_FREED_MAGIC;
        if (s_stats.pages >= frames) s_stats.pages -= frames;
        if (s_stats.used_bytes >= size) s_stats.used_bytes -= size;
        pmm_free_contiguous_frames(paging_kernel_virt_to_phys(large), frames);
        return;
    }
    if (large->magic == KALLOC_FREED_MAGIC) {
        s_stats.double_frees++;
        terminal_puts("kalloc: double free\n");
        return;
    }

    kalloc_slab_page_t* slab = (kalloc_slab_page_t*)((unsigned int)ptr & ~(PAGE_SIZE - 1u));
    if (slab->magic != KALLOC_SLAB_MAGIC ||
        slab->class_index >= KALLOC_CLASS_COUNT ||
        slab->free_blocks >= slab->total_blocks) {
        s_stats.invalid_frees++;
        terminal_puts("kalloc: invalid free\n");
        return;
    }

    *(void**)ptr = slab->free_list;
    slab->free_list = ptr;
    slab->free_blocks++;
    if (s_stats.used_bytes >= slab->block_size) s_stats.used_bytes -= slab->block_size;
    s_stats.free_bytes += slab->block_size;
    slab_remove_if_empty(slab);
}

unsigned int kalloc_stats(kalloc_stats_t* out) {
    if (out) *out = s_stats;
    return s_stats.pages;
}
