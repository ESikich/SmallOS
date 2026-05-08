#ifndef PMM_H
#define PMM_H

#include "types.h"

/*
 * pmm.h — Physical Memory Manager
 *
 * Bitmap-based page frame allocator.
 *
 * Physical memory split:
 *
 *   0x100000 – 0x1FFFFF   bump allocator (kmalloc / kmalloc_page)
 *                          kernel-owned permanent allocations such as
 *                          heap objects and kernel page tables.
 *
 *   0x200000 – 0x7FFFFFF   PMM (this file)
 *                          reclaimable per-process allocations such as
 *                          user ELF frames, user stack frames, process
 *                          page directories, and private page tables.
 *                          126 MB = 32256 frames = 4032 bytes of bitmap.
 *                          E820 limits which frames are actually free.
 *                          pmm_alloc_frame() returns physical addresses;
 *                          kernel code must use paging_phys_to_kernel_virt()
 *                          before dereferencing PMM-backed memory.
 *
 * The two ranges are disjoint — pmm_alloc_frame() and kmalloc_page()
 * can never return the same physical address.
 */

#define PMM_BASE        0x200000u           /* 2 MB  */
#define PMM_LIMIT       0x08000000u         /* 128 MB */
#define PMM_SIZE        (PMM_LIMIT - PMM_BASE)
#define PMM_FALLBACK_SIZE 0x1E00000u        /* 30 MB when E820 is unavailable */
#define PMM_FRAME_SIZE  4096u
#define PMM_NUM_FRAMES  (PMM_SIZE / PMM_FRAME_SIZE)   /* 32256 */

void pmm_init(void);
u32  pmm_alloc_frame(void);
u32  pmm_alloc_contiguous_frames(u32 count);
void pmm_free_frame(u32 addr);
void pmm_free_contiguous_frames(u32 addr, u32 count);
u32  pmm_free_count(void);

#endif /* PMM_H */
