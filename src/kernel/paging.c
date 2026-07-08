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
 * Each process gets a fresh physical page directory from pmm_alloc_frame().
 * Kernel PD entries outside the user range are copied in so that kernel
 * code, VGA, heap, and high device mappings remain accessible after CR3
 * switch.
 *
 * PD indices covering [USER_CODE_BASE, USER_STACK_TOP) are left empty.
 * These cover the fixed ELF slot, PIE main slot, mmap/DSO arena, heap,
 * optional user display mappings, and user stack. Their page tables are
 * allocated from the PMM so process_pd_destroy() can free them and the
 * frames they point to.
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
#define KERNEL_PMM_MAP_PD_INDEX (KERNEL_PMM_MAP_BASE >> 22)
#define KERNEL_PMM_MAP_TABLES   ((KERNEL_PMM_MAP_SIZE + 0x3FFFFFu) >> 22)
#define KERNEL_FB_MAP_PD_INDEX  (0xD0000000u >> 22)
#define KERNEL_HIGH_MMIO_PD_INDEX (0xFE000000u >> 22)
#define KERNEL_HIGH_MMIO_TABLES 8u

#define USER_PD_FIRST_INDEX (USER_CODE_BASE >> 22)
#define USER_PD_LIMIT_INDEX (USER_STACK_TOP >> 22)

static u32 kernel_page_directory[PD_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static u32 low_page_table_0[PT_ENTRIES]      __attribute__((aligned(PAGE_SIZE)));
static u32 low_page_table_1[PT_ENTRIES]      __attribute__((aligned(PAGE_SIZE)));
static u32 framebuffer_page_table[PT_ENTRIES] __attribute__((aligned(PAGE_SIZE)));
static u32 high_mmio_page_tables[KERNEL_HIGH_MMIO_TABLES][PT_ENTRIES]
    __attribute__((aligned(PAGE_SIZE)));
static u32 pmm_map_page_tables[KERNEL_PMM_MAP_TABLES][PT_ENTRIES]
    __attribute__((aligned(PAGE_SIZE)));

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

static int paging_virt_is_pmm_alias(u32 virt) {
    return virt >= KERNEL_PMM_MAP_BASE && virt < KERNEL_PMM_MAP_END;
}

static u32* paging_pd_virt(u32* pd) {
    if (pd == kernel_page_directory) {
        return pd;
    }
    return (u32*)paging_phys_to_kernel_virt((u32)pd);
}

static u32* paging_pt_virt(u32 phys) {
    if (paging_phys_is_pmm_frame(phys)) {
        return (u32*)paging_phys_to_kernel_virt(phys);
    }
    return (u32*)phys;
}

static int paging_pd_index_is_user(u32 index) {
    return index >= USER_PD_FIRST_INDEX && index < USER_PD_LIMIT_INDEX;
}

static void paging_invalidate(u32 virt) {
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");
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

    /*
     * Permanent empty page tables for late-filled kernel device windows.
     *
     * These PDEs are copied into every process page directory and all copies
     * share the same backing page tables, so later framebuffer/e1000 mappings
     * become visible regardless of the CR3 active during a kernel syscall or
     * interrupt. They also avoid allocating kernel page tables from the bump
     * allocator after PMM is live.
     */
    kernel_page_directory[KERNEL_FB_MAP_PD_INDEX] =
        (u32)framebuffer_page_table | PAGE_PRESENT | PAGE_WRITE;
    for (u32 t = 0; t < KERNEL_HIGH_MMIO_TABLES; t++) {
        kernel_page_directory[KERNEL_HIGH_MMIO_PD_INDEX + t] =
            (u32)high_mmio_page_tables[t] | PAGE_PRESENT | PAGE_WRITE;
    }

    /*
     * High kernel alias for all PMM-managed frames. The low identity map
     * remains for boot compatibility, but PMM frames should be dereferenced
     * through this stable supervisor-only window.
     */
    for (u32 t = 0; t < KERNEL_PMM_MAP_TABLES; t++) {
        for (u32 i = 0; i < PT_ENTRIES; i++) {
            u32 page = t * PT_ENTRIES + i;
            if (page < PMM_NUM_FRAMES) {
                pmm_map_page_tables[t][i] =
                    (PMM_BASE + page * PAGE_SIZE) | PAGE_PRESENT | PAGE_WRITE;
            }
        }
        kernel_page_directory[KERNEL_PMM_MAP_PD_INDEX + t] =
            (u32)pmm_map_page_tables[t] | PAGE_PRESENT | PAGE_WRITE;
    }

    /* All other PD entries stay zero (not present). */

    flush_cr3((u32)kernel_page_directory);
    enable_paging();
}

int paging_phys_is_pmm_frame(u32 phys) {
    return phys >= PMM_BASE && phys < PMM_BASE + PMM_SIZE;
}

void* paging_phys_to_kernel_virt(u32 phys) {
    if (!paging_phys_is_pmm_frame(phys)) {
        paging_panic();
    }
    return (void*)(KERNEL_PMM_MAP_BASE + (phys - PMM_BASE));
}

u32 paging_kernel_virt_to_phys(const void* virt_ptr) {
    u32 virt = (u32)virt_ptr;
    if (!paging_virt_is_pmm_alias(virt)) {
        paging_panic();
    }
    return PMM_BASE + (virt - KERNEL_PMM_MAP_BASE);
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
    u32* pd_virt = paging_pd_virt(pd);

    flags |= PAGE_PRESENT;

    u32* pt;

    if (pd_virt[pd_index] & PAGE_PRESENT) {
        pt = paging_pt_virt(pd_virt[pd_index] & ~0xFFFu);
    } else {
        u32 pt_phys;

        if (pd == kernel_page_directory) {
            pt = (u32*)kmalloc_page();
            if (!pt) paging_panic();
            pt_phys = (u32)pt;
        } else {
            u32 frame = pmm_alloc_frame();
            if (!frame) paging_panic();
            pt_phys = frame;
            pt = (u32*)paging_phys_to_kernel_virt(frame);
        }
        k_memset(pt, 0, PAGE_SIZE);
        pd_virt[pd_index] = pt_phys | PAGE_PRESENT | PAGE_WRITE | PAGE_USER;
    }

    if (pd != kernel_page_directory &&
        (pt[pt_index] & (PAGE_PRESENT | PAGE_USER)) == (PAGE_PRESENT | PAGE_USER)) {
        u32 old_phys = pt[pt_index] & ~0xFFFu;
        if (paging_phys_is_pmm_frame(old_phys)) {
            pmm_free_frame(old_phys);
        }
    }

    pt[pt_index] = (phys & ~0xFFFu) | flags;
    __asm__ __volatile__("invlpg (%0)" : : "r"(virt) : "memory");
}

void paging_map_kernel_range(u32 virt, u32 phys, u32 size, u32 flags) {
    if (size == 0) return;

    u32 virt_start = virt & ~(PAGE_SIZE - 1u);
    u32 phys_start = phys & ~(PAGE_SIZE - 1u);
    u32 offset = virt - virt_start;
    u32 total = PAGE_ALIGN(size + offset);

    for (u32 mapped = 0; mapped < total; mapped += PAGE_SIZE) {
        paging_map_page(kernel_page_directory,
                        virt_start + mapped,
                        phys_start + mapped,
                        flags);
    }
}

u32* paging_get_kernel_pd(void) {
    return kernel_page_directory;
}

/*
 * process_pd_create()
 *
 * Allocates the page directory from the PMM (not the bump allocator) so
 * that process_pd_destroy() can free it on exit, closing the 4 KB-per-
 * user program heap leak that existed when kmalloc_page() was used here.
 *
 * The returned value is the physical PD frame address cast to u32*. Kernel
 * code must translate it through the PMM alias before dereferencing.
 */
u32* process_pd_create(void) {
    u32 frame = pmm_alloc_frame();
    if (!frame) paging_panic();

    u32* pd = (u32*)paging_phys_to_kernel_virt(frame);
    k_memset(pd, 0, PAGE_SIZE);

    /*
     * Copy only kernel-space entries into the process directory. User-space
     * PDEs stay empty until exec, mmap, brk, display mapping, or stack setup
     * installs process-owned mappings.
     */
    for (u32 i = 0; i < PD_ENTRIES; i++) {
        if (paging_pd_index_is_user(i)) continue;
        pd[i] = kernel_page_directory[i];
    }

    return (u32*)frame;
}

u32* process_pd_clone_user(u32* src_pd,
                           int (*page_may_write)(void* ctx, u32 virt),
                           void* ctx) {
    if (!src_pd) return 0;

    u32* dst_pd = process_pd_create();
    if (!dst_pd) return 0;

    u32* src = paging_pd_virt(src_pd);

    for (u32 i = USER_PD_FIRST_INDEX; i < USER_PD_LIMIT_INDEX; i++) {
        if (src[i] == kernel_page_directory[i]) continue;
        if (!(src[i] & PAGE_PRESENT)) continue;

        u32* src_pt = paging_pt_virt(src[i] & ~0xFFFu);
        for (u32 j = 0; j < PT_ENTRIES; j++) {
            if ((src_pt[j] & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) {
                continue;
            }

            u32 src_frame = src_pt[j] & ~0xFFFu;
            u32 src_flags = src_pt[j] & 0xFFFu;

            if (!paging_phys_is_pmm_frame(src_frame)) {
                paging_map_page(dst_pd,
                                (i << 22) | (j << 12),
                                src_frame,
                                src_flags);
                continue;
            }

            if (src_flags & PAGE_SHARED_RO_FILE) {
                if (!pmm_retain_frame(src_frame)) {
                    process_pd_destroy(dst_pd);
                    return 0;
                }
                paging_map_page(dst_pd,
                                (i << 22) | (j << 12),
                                src_frame,
                                src_flags & ~PAGE_WRITE);
                continue;
            }

            u32 virt = (i << 22) | (j << 12);
            int can_write = page_may_write ? page_may_write(ctx, virt) : 0;

            if (src_flags & PAGE_COW) {
                if (!pmm_retain_frame(src_frame)) {
                    process_pd_destroy(dst_pd);
                    return 0;
                }
                paging_map_page(dst_pd,
                                virt,
                                src_frame,
                                src_flags & ~PAGE_WRITE);
                continue;
            }

            if ((src_flags & PAGE_WRITE) || can_write) {
                if (!pmm_retain_frame(src_frame)) {
                    process_pd_destroy(dst_pd);
                    return 0;
                }

                src_pt[j] = src_frame |
                            ((src_flags & ~PAGE_WRITE) | PAGE_COW | PAGE_PRESENT);
                paging_invalidate(virt);
                paging_map_page(dst_pd,
                                virt,
                                src_frame,
                                (src_flags & ~PAGE_WRITE) | PAGE_COW);
                continue;
            }

            if (!pmm_retain_frame(src_frame)) {
                process_pd_destroy(dst_pd);
                return 0;
            }
            paging_map_page(dst_pd,
                            virt,
                            src_frame,
                            src_flags & ~PAGE_WRITE);
        }
    }

    return dst_pd;
}

int paging_user_page_present(u32* pd, u32 virt) {
    u32 phys = 0;
    u32 flags = 0;
    return paging_user_page_get(pd, virt, &phys, &flags);
}

int paging_user_page_get(u32* pd, u32 virt, u32* out_phys, u32* out_flags) {
    if (out_phys) *out_phys = 0;
    if (out_flags) *out_flags = 0;
    if (!pd) return 0;

    u32 page = virt & ~(PAGE_SIZE - 1u);
    u32 pd_index = page >> 22;
    u32 pt_index = (page >> 12) & 0x3FFu;
    if (!paging_pd_index_is_user(pd_index)) return 0;

    u32* pd_virt = paging_pd_virt(pd);
    u32 pde = pd_virt[pd_index];
    if ((pde & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) return 0;

    u32* pt = paging_pt_virt(pde & ~0xFFFu);
    u32 pte = pt[pt_index];
    if ((pte & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) return 0;

    if (out_phys) *out_phys = pte & ~0xFFFu;
    if (out_flags) *out_flags = pte & 0xFFFu;
    return 1;
}

int paging_user_page_set_flags(u32* pd, u32 virt, u32 clear_flags, u32 set_flags) {
    if (!pd) return 0;

    u32 page = virt & ~(PAGE_SIZE - 1u);
    u32 pd_index = page >> 22;
    u32 pt_index = (page >> 12) & 0x3FFu;
    if (!paging_pd_index_is_user(pd_index)) return 0;

    u32* pd_virt = paging_pd_virt(pd);
    u32 pde = pd_virt[pd_index];
    if ((pde & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) return 0;

    u32* pt = paging_pt_virt(pde & ~0xFFFu);
    u32 pte = pt[pt_index];
    if ((pte & (PAGE_PRESENT | PAGE_USER)) != (PAGE_PRESENT | PAGE_USER)) return 0;

    pt[pt_index] = (pte & ~clear_flags) | set_flags | PAGE_PRESENT | PAGE_USER;
    paging_invalidate(page);
    return 1;
}

static int paging_pt_empty(u32* pt) {
    for (u32 i = 0; i < PT_ENTRIES; i++) {
        if (pt[i] & PAGE_PRESENT) return 0;
    }
    return 1;
}

int paging_unmap_user_page(u32* pd, u32 virt) {
    if (!pd) return 0;

    u32 page = virt & ~(PAGE_SIZE - 1u);
    u32 pd_index = page >> 22;
    u32 pt_index = (page >> 12) & 0x3FFu;
    if (!paging_pd_index_is_user(pd_index)) return 0;

    u32* pd_virt = paging_pd_virt(pd);
    u32 pde = pd_virt[pd_index];
    if (!(pde & PAGE_PRESENT)) return 0;
    if (pde == kernel_page_directory[pd_index]) return 0;

    u32 pt_phys = pde & ~0xFFFu;
    u32* pt = paging_pt_virt(pt_phys);
    u32 pte = pt[pt_index];
    if (!(pte & PAGE_PRESENT)) return 0;

    if ((pte & PAGE_USER) && paging_phys_is_pmm_frame(pte & ~0xFFFu)) {
        pmm_release_frame(pte & ~0xFFFu);
    }
    pt[pt_index] = 0;
    paging_invalidate(page);

    if (paging_pt_empty(pt)) {
        pd_virt[pd_index] = 0;
        if (paging_phys_is_pmm_frame(pt_phys)) {
            pmm_release_frame(pt_phys);
        }
    }
    return 1;
}

int paging_resolve_cow_fault(u32* pd, u32 fault_addr) {
    if (!pd) return 0;

    u32 virt = fault_addr & ~(PAGE_SIZE - 1u);
    u32 pd_index = virt >> 22;
    u32 pt_index = (virt >> 12) & 0x3FFu;

    if (!paging_pd_index_is_user(pd_index)) return 0;

    u32* pd_virt = paging_pd_virt(pd);
    u32 pde = pd_virt[pd_index];
    if (!(pde & PAGE_PRESENT)) return 0;

    u32* pt = paging_pt_virt(pde & ~0xFFFu);
    u32 pte = pt[pt_index];
    if ((pte & (PAGE_PRESENT | PAGE_USER | PAGE_COW)) !=
        (PAGE_PRESENT | PAGE_USER | PAGE_COW)) {
        return 0;
    }
    if (pte & PAGE_WRITE) return 0;

    u32 old_frame = pte & ~0xFFFu;
    if (!paging_phys_is_pmm_frame(old_frame)) return 0;

    u32 flags = pte & 0xFFFu;
    u32 refs = pmm_frame_refcount(old_frame);

    if (refs <= 1u) {
        pt[pt_index] = old_frame | (((flags | PAGE_WRITE) & ~PAGE_COW) & ~PAGE_SHARED_RO_FILE);
        paging_invalidate(virt);
        return 1;
    }

    u32 new_frame = pmm_alloc_frame();
    if (!new_frame) return 0;

    k_memcpy(paging_phys_to_kernel_virt(new_frame),
             paging_phys_to_kernel_virt(old_frame),
             PAGE_SIZE);

    pt[pt_index] = new_frame | (((flags | PAGE_WRITE) & ~PAGE_COW) & ~PAGE_SHARED_RO_FILE);
    paging_invalidate(virt);
    pmm_release_frame(old_frame);
    return 1;
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
    u32* pd_virt = paging_pd_virt(pd);

    for (u32 i = USER_PD_FIRST_INDEX; i < USER_PD_LIMIT_INDEX; i++) {
        /* Skip entries shared from the kernel PD. */
        if (pd_virt[i] == kernel_page_directory[i]) continue;

        /* Skip entries that aren't present. */
        if (!(pd_virt[i] & PAGE_PRESENT)) continue;

        u32 pt_phys = pd_virt[i] & ~0xFFFu;
        u32* pt = paging_pt_virt(pt_phys);

        /* Free only user-owned physical frames mapped in this private page table. */
        for (u32 j = 0; j < PT_ENTRIES; j++) {
            if ((pt[j] & (PAGE_PRESENT | PAGE_USER)) == (PAGE_PRESENT | PAGE_USER) &&
                paging_phys_is_pmm_frame(pt[j] & ~0xFFFu)) {
                pmm_free_frame(pt[j] & ~0xFFFu);
            }
        }

        /* Every process-private page table is PMM-backed. */
        pmm_free_frame(pt_phys);

        /* Clear the PDE so a stale CR3 can't reach freed memory. */
        pd_virt[i] = 0;
    }

    /* Free the page directory frame itself — allocated from PMM. */
    pmm_free_frame((u32)pd);
}

unsigned int process_pd_count_private_frames(u32* pd) {
    unsigned int frames = 0;

    if (!pd) return 0;

    u32* pd_virt = paging_pd_virt(pd);
    frames = 1;

    for (u32 i = 0; i < PD_ENTRIES; i++) {
        if (pd_virt[i] == kernel_page_directory[i]) continue;
        if (!(pd_virt[i] & PAGE_PRESENT)) continue;

        u32 pt_phys = pd_virt[i] & ~0xFFFu;
        u32* pt = paging_pt_virt(pt_phys);

        frames++;
        for (u32 j = 0; j < PT_ENTRIES; j++) {
            if ((pt[j] & (PAGE_PRESENT | PAGE_USER)) == (PAGE_PRESENT | PAGE_USER) &&
                paging_phys_is_pmm_frame(pt[j] & ~0xFFFu) &&
                pmm_frame_refcount(pt[j] & ~0xFFFu) <= 1u) {
                frames++;
            }
        }
    }

    return frames;
}

void paging_switch(u32* pd) {
    if (pd == kernel_page_directory) {
        flush_cr3((u32)pd);
    } else if (paging_virt_is_pmm_alias((u32)pd)) {
        flush_cr3(paging_kernel_virt_to_phys(pd));
    } else {
        flush_cr3((u32)pd);
    }
}
