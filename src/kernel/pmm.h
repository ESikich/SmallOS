#ifndef PMM_H
#define PMM_H

/*
 * pmm.h — Physical Memory Manager
 *
 * Bitmap-based page frame allocator.
 *
 * Physical memory split:
 *
 *   0x100000 – 0x1FFFFF   bump allocator (kmalloc / kmalloc_page)
 *                          kernel structures: process PDs, page tables,
 *                          parse buffers, argv.  1 MB headroom.
 *
 *   0x200000 – 0x7FFFFF   PMM (this file)
 *                          user ELF frames, user stack frames.
 *                          6 MB = 1536 frames = 192 bytes of bitmap.
 *                          Ceiling matches the 8 MB identity-map limit,
 *                          so all PMM frames are directly accessible as
 *                          pointers in kernel C code (phys == virt).
 *
 * The two ranges are disjoint — pmm_alloc_frame() and kmalloc_page()
 * can never return the same physical address.
 */

typedef unsigned int u32;

#define PMM_BASE        0x200000u           /* 2 MB  */
#define PMM_SIZE        0x600000u           /* 6 MB  */
#define PMM_FRAME_SIZE  4096u
#define PMM_NUM_FRAMES  (PMM_SIZE / PMM_FRAME_SIZE)   /* 1536 */

void pmm_init(void);
u32  pmm_alloc_frame(void);
void pmm_free_frame(u32 addr);
u32  pmm_free_count(void);

#endif /* PMM_H */