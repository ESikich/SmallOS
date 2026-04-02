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
 * kernel, heap, and stack addresses).
 *
 * Must be called after gdt_init() and before any user process is loaded.
 */
void paging_init(void);

/*
 * paging_map_page(pd, virt, phys, flags)
 *
 * Maps a single 4 KB page in the given page directory.
 *
 *   pd    – virtual/physical address of the page directory to modify
 *   virt  – virtual address (rounded down to page boundary)
 *   phys  – physical address (rounded down to page boundary)
 *   flags – PAGE_PRESENT | PAGE_WRITE | PAGE_USER as needed
 *
 * Page table allocation:
 *   Kernel mappings in the master kernel PD may still use kernel-owned
 *   memory, but any page table created in a process page directory is
 *   allocated from pmm_alloc_frame() so process_pd_destroy() can reclaim
 *   it on exit.
 *
 * Panics on allocation failure.
 */
void paging_map_page(u32* pd, u32 virt, u32 phys, u32 flags);

/*
 * paging_get_kernel_pd()
 *
 * Returns a pointer to the kernel page directory.
 * Used by process_pd_create() to copy kernel mappings into new directories.
 */
u32* paging_get_kernel_pd(void);

/*
 * process_pd_create()
 *
 * Allocate and initialize a new page directory for a user process.
 *
 * Allocation is from pmm_alloc_frame() so that process_pd_destroy() can
 * free the directory itself on exit (no heap leak per runelf).
 *
 * The kernel's identity-mapped entries (PD indices 0 and 2–1023) are
 * copied in so that kernel code, heap, and VGA remain accessible
 * after switching CR3. PD index 1 (0x400000–0x7FFFFF) is left empty so
 * the process gets a private mapping there for its ELF.
 *
 * Returns a pointer to the new page directory (page-aligned, identity-
 * mapped so the value is usable as both a virtual and physical address).
 * Returns 0 (via paging_panic — halts) on allocation failure.
 */
u32* process_pd_create(void);

/*
 * process_pd_destroy(pd)
 *
 * Free all resources privately allocated for this process:
 *
 *   1. For each private PDE (present and not shared from the kernel PD):
 *      a. pmm_free_frame() every present PTE frame (ELF pages, stack page)
 *      b. pmm_free_frame() the private page table itself
 *   2. pmm_free_frame() the page directory frame itself
 *
 * Because all process-private page tables are PMM-backed, no per-process
 * page-table memory is leaked when a task exits.
 *
 * Safe to call with a null pointer (no-op).
 */
void process_pd_destroy(u32* pd);

/*
 * paging_switch(pd)
 *
 * Load a page directory into CR3, flushing the TLB.
 */
void paging_switch(u32* pd);

#endif /* PAGING_H */