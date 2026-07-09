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
 *   0x100000 and up       kernel BSS, then bump allocator
 *                          (kmalloc / kmalloc_page) for kernel-owned
 *                          permanent allocations such as heap objects
 *                          and kernel page tables. Early bump allocations
 *                          are reserved during pmm_init(); later ones call
 *                          pmm_reserve_range() as they grow.
 *
 *   0x200000 – 0xFFFFFFF   PMM (this file)
 *                          reclaimable per-process allocations such as
 *                          user ELF frames, user stack frames, process
 *                          page directories, and private page tables.
 *                          Up to 254 MB = 65024 frames. Metadata is sized at
 *                          boot from E820/fallback availability.
 *                          E820 limits which frames are actually free.
 *                          Kernel bump allocations and the boot stack can
 *                          reserve frames inside this range before PMM hands
 *                          them to reclaimable users.
 *                          pmm_alloc_frame() returns physical addresses;
 *                          kernel code must use paging_phys_to_kernel_virt()
 *                          before dereferencing PMM-backed memory.
 *
 * PMM allocations and bump allocations must not alias.  pmm_reserve_range()
 * is the handoff used by the bump allocator once PMM is online.
 */

#define PMM_BASE        0x200000u           /* 2 MB  */
#define PMM_LIMIT       0x10000000u         /* 256 MB */
#define PMM_SIZE        (PMM_LIMIT - PMM_BASE)
#define PMM_FALLBACK_SIZE 0x1E00000u        /* 30 MB when E820 is unavailable */
#define PMM_FRAME_SIZE  4096u
#define PMM_NUM_FRAMES  (PMM_SIZE / PMM_FRAME_SIZE)   /* max managed frames */

void pmm_init(void);
void pmm_reserve_range(u32 start, u32 end);
u32  pmm_alloc_frame(void);
u32  pmm_alloc_contiguous_frames(u32 count);
int  pmm_retain_frame(u32 addr);
void pmm_release_frame(u32 addr);
void pmm_free_frame(u32 addr);
void pmm_free_contiguous_frames(u32 addr, u32 count);
u32  pmm_frame_refcount(u32 addr);
u32  pmm_free_count(void);
u32  pmm_total_count(void);
u32  pmm_used_count(void);
u32  pmm_refcounted_count(void);
u32  pmm_shared_count(void);
u32  pmm_largest_free_run(void);
u32  pmm_max_physical_addr(void);
u32  pmm_managed_frame_count(void);

#endif /* PMM_H */
