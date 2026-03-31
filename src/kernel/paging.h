#ifndef PAGING_H
#define PAGING_H

typedef unsigned int u32;

#define PAGE_PRESENT    0x001   /* entry is valid */
#define PAGE_WRITE      0x002   /* read/write (clear = read-only) */
#define PAGE_USER       0x004   /* accessible from ring 3 */

#define PAGE_SIZE       4096u
#define PAGE_ALIGN(a)   (((a) + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1))

/*
 * Canonical virtual addresses for user processes.
 *
 * Every user ELF is linked at USER_CODE_BASE regardless of which process
 * is running. The process page directory maps that virtual address to the
 * physical pages allocated for this specific ELF.
 *
 * USER_STACK_TOP is the top of the user stack page. The stack grows down
 * from there.
 */
#define USER_CODE_BASE  0x400000u
#define USER_STACK_TOP  0xC0000000u   /* 3 GB — top of user virtual space */
#define USER_STACK_SIZE PAGE_SIZE     /* one page for now */

/*
 * paging_init()
 *
 * Builds the kernel page directory and enables paging.
 * Identity-maps the first 8 MB (physical == virtual for all current
 * kernel, ramdisk, heap, and stack addresses).
 *
 * Must be called after gdt_init() and before any user process is loaded.
 */
void paging_init(void);

/*
 * paging_map_page(pd, virt, phys, flags)
 *
 * Maps a single 4 KB page in the given page directory.
 *
 *   pd    – physical address of the page directory to modify
 *   virt  – virtual address (rounded down to page boundary)
 *   phys  – physical address (rounded down to page boundary)
 *   flags – PAGE_PRESENT | PAGE_WRITE | PAGE_USER as needed
 *
 * Allocates a page table via kmalloc_page() if one does not already
 * exist for the relevant directory entry. Panics on allocation failure.
 */
void paging_map_page(u32* pd, u32 virt, u32 phys, u32 flags);

/*
 * paging_get_kernel_pd()
 *
 * Returns a pointer to the kernel page directory.
 * Used by process_create() to copy kernel mappings into new directories.
 */
u32* paging_get_kernel_pd(void);

/*
 * process_pd_create()
 *
 * Allocate and initialize a new page directory for a user process.
 *
 * The kernel's identity-mapped entries (PD indices 0 and 2–1023) are
 * copied in so that kernel code, ramdisk, heap, and VGA remain accessible
 * after switching CR3. PD index 1 (0x400000–0x7FFFFF) is left empty so
 * the process gets a private mapping there for its ELF.
 *
 * Returns a pointer to the new page directory (page-aligned, identity-
 * mapped so the value is usable as both a virtual and physical address).
 */
u32* process_pd_create(void);

/*
 * process_pd_destroy(pd)
 *
 * Free all page tables and page frames that were privately allocated for
 * this process (PD index 1 only — kernel entries are shared and not freed).
 *
 * Does NOT free the page directory itself (it came from kmalloc_page and
 * the bump allocator has no free). This is a known limitation until a
 * proper physical memory manager is added.
 */
void process_pd_destroy(u32* pd);

/*
 * paging_switch(pd)
 *
 * Load a page directory into CR3, flushing the TLB.
 */
void paging_switch(u32* pd);

#endif