#include "pmm.h"
#include "boot_info.h"
#include "memory.h"
#include "terminal.h"

/*
 * pmm.c — Physical Memory Manager
 *
 * Bitmap allocator covering PMM_BASE (0x200000) up to the boot-selected
 * managed limit, capped by PMM_LIMIT.
 *
 * Kernel BSS starts at 0x100000.  The bump allocator
 * (kmalloc / kmalloc_page) starts after BSS for kernel-owned permanent
 * allocations.  Early bump allocations are reserved during pmm_init();
 * later bump allocations call pmm_reserve_range() so any pages they cross
 * inside the PMM window cannot be handed to user processes.
 *
 * The PMM owns reclaimable page-frame allocations such
 * as per-process page directories, private page tables, user ELF pages, and
 * user stacks. Kernel bump allocations and the boot stack may occupy frames
 * inside this window; pmm_reserve_boot_ranges() and pmm_reserve_range() keep
 * those frames out of the reclaimable pool.
 *
 * Metadata is allocated from the permanent kernel heap before the PMM is marked
 * ready. E820 initialization starts with all frames used, frees only
 * BIOS-reported usable RAM inside this managed window, then re-marks
 * SmallOS-owned ranges including the metadata itself.
 */
static unsigned char* s_bitmap = 0;
static unsigned short* s_refcount = 0;
static u32           s_bitmap_bytes = 0;
static u32           s_managed_frames = 0;
static u32           s_max_phys = PMM_BASE;
static u32           s_free_count = 0;
static u32           s_total_count = 0;
static u32           s_next_free  = 0;
static int           s_ready      = 0;

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
    if (s_refcount[idx] == 0) {
        s_refcount[idx] = 1;
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
    s_refcount[idx] = 0;
}

static u32 round_down_frame(u32 addr) {
    return addr & ~(PMM_FRAME_SIZE - 1u);
}

static u32 round_up_frame(u32 addr) {
    return (addr + PMM_FRAME_SIZE - 1u) & ~(PMM_FRAME_SIZE - 1u);
}

static void pmm_mark_range_free(u32 start, u32 end) {
    if (end <= PMM_BASE || start >= s_max_phys) {
        return;
    }

    if (start < PMM_BASE) {
        start = PMM_BASE;
    }
    if (end > s_max_phys) {
        end = s_max_phys;
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
    if (end <= PMM_BASE || start >= s_max_phys) {
        return;
    }

    if (start < PMM_BASE) {
        start = PMM_BASE;
    }
    if (end > s_max_phys) {
        end = s_max_phys;
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
        if (end64 <= PMM_BASE || ent->base >= (u64)s_max_phys) {
            continue;
        }

        u32 start = ent->base < PMM_BASE ? PMM_BASE : (u32)ent->base;
        u32 end = end64 > (u64)s_max_phys ? s_max_phys : (u32)end64;
        pmm_mark_range_free(start, end);
    }
}

static void pmm_free_fixed_range(void) {
    pmm_mark_range_free(PMM_BASE, PMM_BASE + PMM_FALLBACK_SIZE);
}

static u32 pmm_detect_max_phys(void) {
    u32 max_phys = PMM_BASE + PMM_FALLBACK_SIZE;

    if (boot_info_e820_valid()) {
        const boot_info_t* info = boot_info_get();
        max_phys = PMM_BASE;
        for (u32 i = 0; i < info->e820_count; i++) {
            const boot_e820_entry_t* ent = &info->e820[i];
            u64 end64;
            if (ent->type != 1u || ent->length == 0) continue;
            end64 = ent->base + ent->length;
            if (end64 <= PMM_BASE) continue;
            if (ent->base >= (u64)PMM_LIMIT) continue;
            if (end64 > (u64)PMM_LIMIT) end64 = PMM_LIMIT;
            if ((u32)end64 > max_phys) max_phys = (u32)end64;
        }
        if (max_phys <= PMM_BASE) {
            max_phys = PMM_BASE + PMM_FALLBACK_SIZE;
        }
    }

    if (max_phys > PMM_LIMIT) max_phys = PMM_LIMIT;
    max_phys &= ~(PMM_FRAME_SIZE - 1u);
    if (max_phys <= PMM_BASE) max_phys = PMM_BASE + PMM_FRAME_SIZE;
    return max_phys;
}

static void pmm_metadata_init(void) {
    s_max_phys = pmm_detect_max_phys();
    s_managed_frames = (s_max_phys - PMM_BASE) / PMM_FRAME_SIZE;
    if (s_managed_frames > PMM_NUM_FRAMES) {
        s_managed_frames = PMM_NUM_FRAMES;
        s_max_phys = PMM_BASE + s_managed_frames * PMM_FRAME_SIZE;
    }
    s_bitmap_bytes = (s_managed_frames + 7u) / 8u;
    s_bitmap = (unsigned char*)kmalloc(s_bitmap_bytes);
    s_refcount = (unsigned short*)kmalloc(s_managed_frames * sizeof(unsigned short));
    if (!s_bitmap || !s_refcount) {
        terminal_puts("pmm: metadata allocation failed\n");
        for (;;) {
            __asm__ __volatile__("cli; hlt");
        }
    }
}

static void pmm_reserve_boot_ranges(void) {
    const boot_info_t* info = boot_info_get();

    pmm_mark_range_used(0x00000000u, 0x00001000u);
    pmm_mark_range_used(0x00007C00u, 0x00007E00u);
    pmm_mark_range_used(0x00080000u, 0x00082000u);
    pmm_mark_range_used(BOOT_INFO_PHYS, BOOT_INFO_PHYS + sizeof(boot_info_t));
    pmm_mark_range_used(BOOT_FONT_PHYS, BOOT_FONT_PHYS + 0x1000u);
    pmm_mark_range_used(KERNEL_BOOT_STACK_TOP - PMM_FRAME_SIZE,
                        KERNEL_BOOT_STACK_TOP);
    pmm_mark_range_used(0x00100000u, memory_get_heap_top());

    if (boot_info_framebuffer_valid()) {
        u32 fb_bytes = info->framebuffer_pitch * info->framebuffer_height;
        pmm_mark_range_used(info->framebuffer_phys,
                            info->framebuffer_phys + fb_bytes);
    }

    if (boot_info_ramdisk_valid()) {
        pmm_mark_range_used(info->ramdisk_phys,
                            info->ramdisk_phys + info->ramdisk_size);
    }
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void pmm_init(void) {
    pmm_metadata_init();

    for (u32 i = 0; i < s_bitmap_bytes; i++) {
        s_bitmap[i] = 0xFFu;
    }
    for (u32 i = 0; i < s_managed_frames; i++) {
        s_refcount[i] = 1;
    }

    s_free_count = 0;
    s_total_count = 0;
    s_next_free  = 0;
    s_ready = 0;

    if (boot_info_e820_valid()) {
        pmm_free_e820_usable_ranges(boot_info_get());
    } else {
        pmm_free_fixed_range();
    }

    pmm_reserve_boot_ranges();
    s_ready = 1;
    s_total_count = s_free_count;

    terminal_puts("pmm: ");
    terminal_put_uint(s_free_count);
    terminal_puts(" frames free (");
    terminal_put_uint(s_free_count * PMM_FRAME_SIZE / 1024);
    terminal_puts(" KB), managed up to ");
    terminal_put_hex(s_max_phys);
    terminal_putc('\n');
}

void pmm_reserve_range(u32 start, u32 end) {
    if (!s_ready) {
        return;
    }

    pmm_mark_range_used(start, end);
}

u32 pmm_alloc_frame(void) {
    if (s_free_count == 0) {
        terminal_puts("pmm: out of frames!\n");
        return 0;
    }

    for (u32 i = 0; i < s_managed_frames; i++) {
        u32 idx = (s_next_free + i) % s_managed_frames;
        if (!frame_is_used(idx)) {
            frame_mark_used(idx);
            s_refcount[idx] = 1;
            s_free_count--;
            s_next_free = (idx + 1) % s_managed_frames;
            return idx_to_addr(idx);
        }
    }

    terminal_puts("pmm: bitmap inconsistency!\n");
    return 0;
}

u32 pmm_alloc_contiguous_frames(u32 count) {
    if (count == 0 || count > s_free_count || count > s_managed_frames) {
        return 0;
    }

    for (u32 start = 0; start + count <= s_managed_frames; start++) {
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
            s_refcount[start + off] = 1;
        }
        s_free_count -= count;
        s_next_free = (start + count) % s_managed_frames;
        return idx_to_addr(start);
    }

    terminal_puts("pmm: no contiguous frame run\n");
    return 0;
}

int pmm_retain_frame(u32 addr) {
    if (addr < PMM_BASE || addr >= s_max_phys) {
        return 0;
    }

    u32 idx = addr_to_idx(addr);

    if (!frame_is_used(idx)) {
        terminal_puts("pmm: retain free frame at ");
        terminal_put_hex(addr);
        terminal_putc('\n');
        return 0;
    }
    if (s_refcount[idx] == 0xFFFFu) {
        terminal_puts("pmm: refcount overflow at ");
        terminal_put_hex(addr);
        terminal_putc('\n');
        return 0;
    }
    s_refcount[idx]++;
    return 1;
}

void pmm_release_frame(u32 addr) {
    if (addr < PMM_BASE || addr >= s_max_phys) {
        return;
    }

    u32 idx = addr_to_idx(addr);

    if (!frame_is_used(idx) || s_refcount[idx] == 0) {
        terminal_puts("pmm: double free at ");
        terminal_put_hex(addr);
        terminal_putc('\n');
        return;
    }

    s_refcount[idx]--;
    if (s_refcount[idx] == 0) {
        frame_mark_free(idx);
        s_free_count++;

        if (idx < s_next_free) {
            s_next_free = idx;
        }
    }
}

void pmm_free_frame(u32 addr) {
    pmm_release_frame(addr);
}

void pmm_free_contiguous_frames(u32 addr, u32 count) {
    if (count == 0) {
        return;
    }

    for (u32 off = 0; off < count; off++) {
        pmm_free_frame(addr + off * PMM_FRAME_SIZE);
    }
}

u32 pmm_frame_refcount(u32 addr) {
    if (addr < PMM_BASE || addr >= s_max_phys) {
        return 0;
    }
    return s_refcount[addr_to_idx(addr)];
}

u32 pmm_free_count(void) {
    return s_free_count;
}

u32 pmm_total_count(void) {
    return s_total_count;
}

u32 pmm_used_count(void) {
    return s_total_count >= s_free_count ? s_total_count - s_free_count : 0u;
}

u32 pmm_refcounted_count(void) {
    return pmm_used_count();
}

u32 pmm_shared_count(void) {
    u32 count = 0;
    for (u32 i = 0; i < s_managed_frames; i++) {
        if (frame_is_used(i) && s_refcount[i] > 1u) count++;
    }
    return count;
}

u32 pmm_largest_free_run(void) {
    u32 best = 0;
    u32 cur = 0;
    for (u32 i = 0; i < s_managed_frames; i++) {
        if (!frame_is_used(i)) {
            cur++;
            if (cur > best) best = cur;
        } else {
            cur = 0;
        }
    }
    return best;
}

u32 pmm_max_physical_addr(void) {
    return s_max_phys;
}

u32 pmm_managed_frame_count(void) {
    return s_managed_frames;
}
