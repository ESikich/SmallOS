#include "pmm.h"
#include "terminal.h"

/*
 * pmm.c — Physical Memory Manager
 *
 * Bitmap allocator covering PMM_BASE (0x200000) .. 0x7FFFFF.
 *
 * The bump allocator (kmalloc / kmalloc_page) owns 0x100000–0x1FFFFF.
 * The PMM owns 0x200000–0x7FFFFF. The ranges never overlap, so there
 * is no ordering constraint between the two allocators.
 *
 * Bitmap: 1536 bits = 192 bytes, static in BSS (zeroed before kernel_main).
 * All frames start free (bit = 0).
 */

 /* Bitmap: 1536 bits = 192 bytes, static in BSS (zeroed before kernel_main). */
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

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

void pmm_init(void) {
    /* BSS is already zeroed — all frames are free at entry. */
    s_free_count = PMM_NUM_FRAMES;
    s_next_free  = 0;

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

u32 pmm_free_count(void) {
    return s_free_count;
}