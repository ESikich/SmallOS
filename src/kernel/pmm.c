#include "pmm.h"
#include "boot_info.h"
#include "memory.h"
#include "terminal.h"

/*
 * pmm.c — Physical Memory Manager
 *
 * Bitmap allocator covering PMM_BASE (0x200000) .. 0x7FFFFFF.
 *
 * Kernel BSS starts at 0x100000.  The bump allocator
 * (kmalloc / kmalloc_page) starts after BSS and owns the remaining
 * space before the boot stack for kernel-owned permanent allocations.
 * The PMM owns 0x200000–0x7FFFFFF for reclaimable page-frame allocations such as
 * per-process page directories, private page tables, user ELF pages,
 * and user stacks.  The ranges never overlap, so there is no ordering
 * constraint between the two allocators.
 *
 * Bitmap: 32256 bits = 4032 bytes, static in BSS (zeroed before kernel_main).
 * E820 initialization starts with all frames used, frees only BIOS-reported
 * usable RAM inside this fixed window, then re-marks SmallOS-owned ranges.
 */
static unsigned char s_bitmap[PMM_NUM_FRAMES / 8];
static u32           s_free_count = 0;
static u32           s_next_free  = 0;

/* ------------------------------------------------------------------ */
/* Bitmap helpers                                                       */
/* ------------------------------------------------------------------ */

static int frame_is_used(u32 idx) {
    return (s_bitmap[idx / 8] >> (idx % 8)) & 1;
}

static void frame_mark_used(u32 idx) {
    s_bitmap[idx / 8] |= (unsigned char)(1u << (idx % 8));
}

static void frame_mark_free(u32 idx) {
    s_bitmap[idx / 8] &= (unsigned char)~(1u << (idx % 8));
}

static u32 addr_to_idx(u32 addr) {
    return (addr - PMM_BASE) / PMM_FRAME_SIZE;
}

static u32 idx_to_addr(u32 idx) {
    return PMM_BASE + idx * PMM_FRAME_SIZE;
}

static void pmm_mark_idx_used(u32 idx) {
    if (!frame_is_used(idx)) {
        frame_mark_used(idx);
        if (s_free_count > 0) {
            s_free_count--;
        }
    }
}

static void pmm_mark_idx_free(u32 idx) {
    if (frame_is_used(idx)) {
        frame_mark_free(idx);
        s_free_count++;
        if (idx < s_next_free) {
            s_next_free = idx;
        }
    }
}

static u32 round_down_frame(u32 addr) {
    return addr & ~(PMM_FRAME_SIZE - 1u);
}

static u32 round_up_frame(u32 addr) {
    return (addr + PMM_FRAME_SIZE - 1u) & ~(PMM_FRAME_SIZE - 1u);
}

static void pmm_mark_range_free(u32 start, u32 end) {
    if (end <= PMM_BASE || start >= PMM_BASE + PMM_SIZE) {
        return;
    }

    if (start < PMM_BASE) {
        start = PMM_BASE;
    }
    if (end > PMM_BASE + PMM_SIZE) {
        end = PMM_BASE + PMM_SIZE;
    }

    start = round_up_frame(start);
    end = round_down_frame(end);
    if (end <= start) {
        return;
    }

    for (u32 addr = start; addr < end; addr += PMM_FRAME_SIZE) {
        pmm_mark_idx_free(addr_to_idx(addr));
    }
}

static void pmm_mark_range_used(u32 start, u32 end) {
    if (end <= PMM_BASE || start >= PMM_BASE + PMM_SIZE) {
        return;
    }

    if (start < PMM_BASE) {
        start = PMM_BASE;
    }
    if (end > PMM_BASE + PMM_SIZE) {
        end = PMM_BASE + PMM_SIZE;
    }

    start = round_down_frame(start);
    end = round_up_frame(end);
    if (end <= start) {
        return;
    }

    for (u32 addr = start; addr < end; addr += PMM_FRAME_SIZE) {
        pmm_mark_idx_used(addr_to_idx(addr));
    }
}

static void pmm_free_e820_usable_ranges(const boot_info_t* info) {
    for (u32 i = 0; i < info->e820_count; i++) {
        const boot_e820_entry_t* ent = &info->e820[i];
        u64 end64;

        if (ent->type != 1u || ent->length == 0) {
            continue;
        }

        end64 = ent->base + ent->length;
        if (end64 <= PMM_BASE || ent->base >= (u64)(PMM_BASE + PMM_SIZE)) {
            continue;
        }

        u32 start = ent->base < PMM_BASE ? PMM_BASE : (u32)ent->base;
        u32 end = end64 > (u64)(PMM_BASE + PMM_SIZE) ?
                  PMM_BASE + PMM_SIZE : (u32)end64;
        pmm_mark_range_free(start, end);
    }
}

static void pmm_free_fixed_range(void) {
    pmm_mark_range_free(PMM_BASE, PMM_BASE + PMM_FALLBACK_SIZE);
}

static void pmm_reserve_boot_ranges(void) {
    const boot_info_t* info = boot_info_get();

    pmm_mark_range_used(0x00000000u, 0x00001000u);
    pmm_mark_range_used(0x00007C00u, 0x00007E00u);
    pmm_mark_range_used(0x00040000u, 0x00041000u);
    pmm_mark_range_used(BOOT_INFO_PHYS, BOOT_INFO_PHYS + sizeof(boot_info_t));
    pmm_mark_range_used(BOOT_FONT_PHYS, BOOT_FONT_PHYS + 0x1000u);
    pmm_mark_range_used(0x001FF000u, 0x00200000u);
    pmm_mark_range_used(0x00100000u, memory_get_heap_top());

    if (boot_info_framebuffer_valid()) {
        u32 fb_bytes = info->framebuffer_pitch * info->framebuffer_height;
        pmm_mark_range_used(info->framebuffer_phys,
                            info->framebuffer_phys + fb_bytes);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void pmm_init(void) {
    for (u32 i = 0; i < sizeof(s_bitmap); i++) {
        s_bitmap[i] = 0xFFu;
    }

    s_free_count = 0;
    s_next_free  = 0;

    if (boot_info_e820_valid()) {
        pmm_free_e820_usable_ranges(boot_info_get());
    } else {
        pmm_free_fixed_range();
    }

    pmm_reserve_boot_ranges();

    terminal_puts("pmm: ");
    terminal_put_uint(s_free_count);
    terminal_puts(" frames free (");
    terminal_put_uint(s_free_count * PMM_FRAME_SIZE / 1024);
    terminal_puts(" KB)\n");
}

u32 pmm_alloc_frame(void) {
    if (s_free_count == 0) {
        terminal_puts("pmm: out of frames!\n");
        return 0;
    }

    for (u32 i = 0; i < PMM_NUM_FRAMES; i++) {
        u32 idx = (s_next_free + i) % PMM_NUM_FRAMES;
        if (!frame_is_used(idx)) {
            frame_mark_used(idx);
            s_free_count--;
            s_next_free = (idx + 1) % PMM_NUM_FRAMES;
            return idx_to_addr(idx);
        }
    }

    terminal_puts("pmm: bitmap inconsistency!\n");
    return 0;
}

u32 pmm_alloc_contiguous_frames(u32 count) {
    if (count == 0 || count > s_free_count || count > PMM_NUM_FRAMES) {
        return 0;
    }

    for (u32 start = 0; start + count <= PMM_NUM_FRAMES; start++) {
        int free_run = 1;

        for (u32 off = 0; off < count; off++) {
            if (frame_is_used(start + off)) {
                free_run = 0;
                start += off;
                break;
            }
        }

        if (!free_run) {
            continue;
        }

        for (u32 off = 0; off < count; off++) {
            frame_mark_used(start + off);
        }
        s_free_count -= count;
        s_next_free = (start + count) % PMM_NUM_FRAMES;
        return idx_to_addr(start);
    }

    terminal_puts("pmm: no contiguous frame run\n");
    return 0;
}

void pmm_free_frame(u32 addr) {
    if (addr < PMM_BASE || addr >= PMM_BASE + PMM_SIZE) {
        return;
    }

    u32 idx = addr_to_idx(addr);

    if (!frame_is_used(idx)) {
        terminal_puts("pmm: double free at ");
        terminal_put_hex(addr);
        terminal_putc('\n');
        return;
    }

    frame_mark_free(idx);
    s_free_count++;

    if (idx < s_next_free) {
        s_next_free = idx;
    }
}

void pmm_free_contiguous_frames(u32 addr, u32 count) {
    if (count == 0) {
        return;
    }

    for (u32 off = 0; off < count; off++) {
        pmm_free_frame(addr + off * PMM_FRAME_SIZE);
    }
}

u32 pmm_free_count(void) {
    return s_free_count;
}
