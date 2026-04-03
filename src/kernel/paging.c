#include "paging.h"
#include "memory.h"
#include "pmm.h"
#include "klib.h"

/*
 * paging.c — Paging subsystem
 *
 * Kernel page directory
 * ---------------------
 * Three static arrays in .bss, zeroed by kernel_entry.asm:
 *
 *   kernel_page_directory[1024]   — the master PD
 *   low_page_table_0[1024]        — identity maps 0x000000–0x3FFFFF (PD 0)
 *   low_page_table_1[1024]        — identity maps 0x400000–0x7FFFFF (PD 1)
 *
 * Per-process page directories
 * ----------------------------
 * Each process gets a fresh page directory from pmm_alloc_frame().
 * Kernel PD entries (indices 0 and 2–1023) are copied in so that kernel
 * code, VGA, heap, and stack remain accessible after CR3 switch.
 *
 * PD index 1 (virtual 0x400000–0x7FFFFF) is left empty — this is the
 * private ELF region. Its page table is allocated from the PMM so that
 * process_pd_destroy() can free it and the frames it points to.
 *
 * The user stack lives at 0xBFFFF000 (PD index 767). Its page table is
 * allocated from pmm_alloc_frame() so process_pd_destroy() can free it
 * along with the stack frame it maps.
 *
 * User ELFs must be linked with -Ttext-segment 0x400000 (not -Ttext) so
 * that the PT_LOAD segment itself starts at 0x400000 (PD index 1). Using
 * -Ttext places .text at 0x400000 but the linker inserts a preceding header
 * segment at 0x3FF000 (PD index 0), which shares the kernel page table and
 * is never reclaimed by process_pd_destroy().
 */

#define PD_ENTRIES  1024
#define PT_ENTRIES  1024

/* PD index 1 covers 0x400000–0x7FFFFF — the private ELF region. */
#define USER_PD_INDEX   1

static u32 kernel_page_directory[PD_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static u32 low_page_table_0[PT_ENTRIES]      __attribute__((aligned(PAGE_SIZE)));
static u32 low_page_table_1[PT_ENTRIES]      __attribute__((aligned(PAGE_SIZE)));

/* ------------------------------------------------------------------ */
/* Internal helpers                                                     */
/* ------------------------------------------------------------------ */

static void paging_panic(void) {
    volatile unsigned char* vga = (volatile unsigned char*)0xB8000;
    vga[0] = 'P'; vga[1] = 0x4F;
    __asm__ __volatile__("cli");
    for (;;) { __asm__ __volatile__("hlt"); }
}

static void flush_cr3(u32 pd_phys) {
    __asm__ __volatile__("mov %0, %%cr3" : : "r"(pd_phys) : "memory");
}

static void enable_paging(void) {
    u32 cr0;
    __asm__ __volatile__("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000u;
    __asm__ __volatile__("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void paging_init(void) {
    /* PT 0: identity-map 0x000000–0x3FFFFF (PD index 0, supervisor) */
    for (u32 i = 0; i < PT_ENTRIES; i++) {
        low_page_table_0[i] = (i * PAGE_SIZE) | PAGE_PRESENT | PAGE_WRITE;
    }
    kernel_page_directory[0] = (u32)low_page_table_0 | PAGE_PRESENT | PAGE_WRITE;

    /* PT 1: identity-map 0x400000–0x7FFFFF (PD index 1, supervisor) */
    for (u32 i = 0; i < PT_ENTRIES; i++) {
        low_page_table_1[i] = (0x400000 + i * PAGE_SIZE) | PAGE_PRESENT | PAGE_WRITE;
    }
    kernel_page_directory[1] = (u32)low_page_table_1 | PAGE_PRESENT | PAGE_WRITE;

    /* All other PD entries stay zero (not present). */

    flush_cr3((u32)kernel_page_directory);
    enable_paging();
}

/*
 * paging_map_page(pd, virt, phys, flags)
 *
 * Maps a single 4 KB page in the given page directory.
 *
 * Allocation policy:
 *   - If the page table already exists, reuse it.
 *   - If we are extending a process page directory, allocate the new page
 *     table from the PMM so process_pd_destroy() can reclaim it later.
 *   - If we are extending the master kernel page directory, allocate from
 *     kmalloc_page() because kernel mappings are permanent in this design.
 */
void paging_map_page(u32* pd, u32 virt, u32 phys, u32 flags) {
    u32 pd_index = virt >> 22;
    u32 pt_index = (virt >> 12) & 0x3FF;

    flags |= PAGE_PRESENT;

    u32* pt;

    if (pd[pd_index] & PAGE_PRESENT) {
        pt = (u32*)(pd[pd_index] & ~0xFFFu);
    } else {
        if (pd == kernel_page_directory) {
            pt = (u32*)kmalloc_page();
            if (!pt) paging_panic();
        } else {
            u32 frame = pmm_alloc_frame();
            if (!frame) paging_panic();
            pt = (u32*)frame;
        }
        k_memset(pt, 0, PAGE_SIZE);
        pd[pd_index] = (u32)pt | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }

    pt[pt_index] = (phys & ~0xFFFu) | flags;
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");
}

u32* paging_get_kernel_pd(void) {
    return kernel_page_directory;
}

/*
 * process_pd_create()
 *
 * Allocates the page directory from the PMM (not the bump allocator) so
 * that process_pd_destroy() can free it on exit, closing the 4 KB-per-
 * runelf heap leak that existed when kmalloc_page() was used here.
 *
 * The PD frame is identity-mapped (phys == virt), so the returned pointer
 * is valid for both CR3 loads and direct kernel writes.
 */
u32* process_pd_create(void) {
    u32 frame = pmm_alloc_frame();
    if (!frame) paging_panic();

    u32* pd = (u32*)frame;
    k_memset(pd, 0, PAGE_SIZE);

    /*
     * Copy kernel PD entries into the process directory so that kernel
     * code, VGA, heap, and stack remain accessible after CR3
     * switch. We skip PD index USER_PD_INDEX (1) — that's the private
     * ELF region and each process gets its own mapping there.
     */
    for (u32 i = 0; i < PD_ENTRIES; i++) {
        if (i == USER_PD_INDEX) continue;
        pd[i] = kernel_page_directory[i];
    }

    return pd;
}

/*
 * process_pd_destroy(pd)
 *
 * Frees all physical frames privately allocated for this process, the
 * page tables that mapped them, and the page directory itself.
 *
 * Strategy: walk every PD entry that is present AND differs from the kernel
 * PD entry at the same index.  Those are the entries the process owns
 * privately (as opposed to entries shared from the kernel PD).
 *
 * For each such private PDE:
 *   - Walk the page table and call pmm_free_frame() on every present PTE's
 *     physical frame.  These frames were allocated from the PMM by the ELF
 *     loader (ELF segment pages) or by elf_run_image (stack frame).
 *   - Free the page table itself. Any page table private to a process now
 *     comes from the PMM.
 *
 * After walking all entries, the PD frame itself is freed.  It was
 * allocated from the PMM by process_pd_create(), so pmm_free_frame() is
 * the correct reclaim path.
 *
 * Kernel PD entries (shared) are never touched.
 */
void process_pd_destroy(u32* pd) {
    if (!pd) return;

    for (u32 i = 0; i < PD_ENTRIES; i++) {
        /* Skip entries shared from the kernel PD. */
        if (pd[i] == kernel_page_directory[i]) continue;

        /* Skip entries that aren't present. */
        if (!(pd[i] & PAGE_PRESENT)) continue;

        u32* pt = (u32*)(pd[i] & ~0xFFFu);

        /* Free every physical frame mapped in this private page table. */
        for (u32 j = 0; j < PT_ENTRIES; j++) {
            if (pt[j] & PAGE_PRESENT) {
                pmm_free_frame(pt[j] & ~0xFFFu);
            }
        }

        /* Every process-private page table is PMM-backed. */
        pmm_free_frame((u32)pt);

        /* Clear the PDE so a stale CR3 can't reach freed memory. */
        pd[i] = 0;
    }

    /* Free the page directory frame itself — allocated from PMM. */
    pmm_free_frame((u32)pd);
}

void paging_switch(u32* pd) {
    flush_cr3((u32)pd);
}