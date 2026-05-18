#include "fb_console.h"
#include "terminal.h"
#include "boot_info.h"
#include "paging.h"
#include "memory.h"
#include "klib.h"
#include "cpu.h"

#define FB_FONT_WIDTH 8u
#define FB_FONT_HEIGHT 16u
#define FB_MAX_COLS 160
#define FB_MAX_ROWS 64
#define FB_MAX_BYTES (16u * 1024u * 1024u)
#define FB_COLOR_BG 0x00000000u
#define FB_COLOR_FG 0x00FFFFFFu

typedef struct {
    volatile u8* base;
    u32 phys;
    u32 width;
    u32 height;
    u32 pitch;
    u32 bpp;
    int rows;
    int cols;
    int row;
    int col;
    int ready;
    int update_depth;
    int dirty_min_row;
    int dirty_max_row;
    unsigned char dirty[FB_MAX_ROWS][FB_MAX_COLS];
    char cells[FB_MAX_ROWS][FB_MAX_COLS];
} fb_state_t;

static fb_state_t* fb;

static const u8* fb_font(void) {
    return (const u8*)BOOT_FONT_PHYS;
}

static void fb_dirty_reset(void) {
    fb->dirty_min_row = fb->rows;
    fb->dirty_max_row = -1;
    for (int r = 0; r < fb->rows; r++) {
        for (int c = 0; c < fb->cols; c++) {
            fb->dirty[r][c] = 0;
        }
    }
}

static void fb_dirty_cell(int r, int c) {
    if (r < 0 || c < 0 || r >= fb->rows || c >= fb->cols) {
        return;
    }

    fb->dirty[r][c] = 1;
    if (r < fb->dirty_min_row) fb->dirty_min_row = r;
    if (r > fb->dirty_max_row) fb->dirty_max_row = r;
}

static void fb_dirty_all(void) {
    fb->dirty_min_row = 0;
    fb->dirty_max_row = fb->rows - 1;
    for (int r = 0; r < fb->rows; r++) {
        for (int c = 0; c < fb->cols; c++) {
            fb->dirty[r][c] = 1;
        }
    }
}

static void fb_store_words(volatile u32* dst, u32 value, u32 count) {
    void* out = (void*)dst;

    __asm__ __volatile__(
        "cld\n\t"
        "rep stosl"
        : "+D"(out), "+c"(count)
        : "a"(value)
        : "memory");
}

static void fb_copy_words(volatile u32* dst, const u32* src, u32 count) {
    void* out = (void*)dst;
    const void* in = (const void*)src;

    __asm__ __volatile__(
        "cld\n\t"
        "rep movsl"
        : "+D"(out), "+S"(in), "+c"(count)
        :
        : "memory");
}

static void fb_clear_rect(u32 x, u32 y, u32 w, u32 h) {
    for (u32 py = 0; py < h && y + py < fb->height; py++) {
        volatile u32* row = (volatile u32*)(fb->base + (y + py) * fb->pitch + x * 4u);
        u32 count = w;
        if (count > fb->width - x) count = fb->width - x;
        fb_store_words(row, FB_COLOR_BG, count);
    }
}

static void fb_draw_cell(int r, int c) {
    if (!fb->ready || r < 0 || c < 0 || r >= fb->rows || c >= fb->cols) {
        return;
    }

    unsigned char ch = (unsigned char)fb->cells[r][c];
    const u8* glyph = fb_font() + (u32)ch * FB_FONT_HEIGHT;
    u32 px0 = (u32)c * FB_FONT_WIDTH;
    u32 py0 = (u32)r * FB_FONT_HEIGHT;

    for (u32 py = 0; py < FB_FONT_HEIGHT; py++) {
        u8 bits = glyph[py];
        volatile u32* out = (volatile u32*)(fb->base + (py0 + py) * fb->pitch + px0 * 4u);
        for (u32 px = 0; px < FB_FONT_WIDTH; px++) {
            u32 mask = 0x80u >> px;
            out[px] = (bits & mask) ? FB_COLOR_FG : FB_COLOR_BG;
        }
    }
}

static void fb_draw_cursor(void) {
    if (!fb->ready || fb->row < 0 || fb->col < 0 ||
        fb->row >= fb->rows || fb->col >= fb->cols) {
        return;
    }

    u32 px0 = (u32)fb->col * FB_FONT_WIDTH;
    u32 py0 = (u32)fb->row * FB_FONT_HEIGHT + FB_FONT_HEIGHT - 2u;

    for (u32 py = 0; py < 2u; py++) {
        volatile u32* out = (volatile u32*)(fb->base + (py0 + py) * fb->pitch + px0 * 4u);
        fb_store_words(out, FB_COLOR_FG, FB_FONT_WIDTH);
    }
}

static void fb_erase_cursor(void) {
    fb_draw_cell(fb->row, fb->col);
}

static void fb_erase_cursor_if_visible(void) {
    if (fb->update_depth == 0) {
        fb_erase_cursor();
    }
}

static void fb_draw_cursor_if_visible(void) {
    if (fb->update_depth == 0) {
        fb_draw_cursor();
    }
}

static void fb_scroll_cells(void) {
    for (int r = 1; r < fb->rows; r++) {
        k_memcpy(fb->cells[r - 1], fb->cells[r], (k_size_t)fb->cols);
    }

    for (int c = 0; c < fb->cols; c++) {
        fb->cells[fb->rows - 1][c] = ' ';
    }
}

static void fb_scroll(void) {
    if (fb->update_depth > 0) {
        fb_scroll_cells();
        fb_dirty_all();
        fb->row = fb->rows - 1;
        fb->col = 0;
        return;
    }

    u32 row_bytes = fb->pitch * FB_FONT_HEIGHT;
    u32 copy_bytes = row_bytes * (u32)(fb->rows - 1);
    volatile u8* dst = fb->base;
    volatile u8* src = fb->base + row_bytes;
    u32 words = copy_bytes / 4u;
    u32 bytes = copy_bytes & 3u;

    fb_copy_words((volatile u32*)dst, (const u32*)src, words);
    for (u32 i = 0; i < bytes; i++) {
        dst[words * 4u + i] = src[words * 4u + i];
    }

    fb_scroll_cells();

    fb_clear_rect(0, (u32)(fb->rows - 1) * FB_FONT_HEIGHT,
                  fb->width, FB_FONT_HEIGHT);
    fb->row = fb->rows - 1;
    fb->col = 0;
}

static void fb_clear(void) {
    if (!fb->ready) {
        return;
    }

    fb_clear_rect(0, 0, fb->width, fb->height);
    for (int r = 0; r < fb->rows; r++) {
        for (int c = 0; c < fb->cols; c++) {
            fb->cells[r][c] = ' ';
        }
    }

    fb->row = 0;
    fb->col = 0;
    fb_dirty_reset();
    fb_draw_cursor();
}

static void fb_flush_dirty(void) {
    if (fb->dirty_max_row < fb->dirty_min_row) {
        return;
    }

    for (int r = fb->dirty_min_row; r <= fb->dirty_max_row; r++) {
        for (int c = 0; c < fb->cols; c++) {
            if (fb->dirty[r][c]) {
                fb_draw_cell(r, c);
            }
        }
    }
    fb_dirty_reset();
}

static void fb_putc(char c) {
    if (!fb->ready) {
        return;
    }

    fb_erase_cursor_if_visible();

    if (c == '\n') {
        fb->col = 0;
        fb->row++;
    } else if (c == '\r') {
        fb->col = 0;
    } else if (c == '\b') {
        if (fb->col > 0) {
            fb->col--;
            fb->cells[fb->row][fb->col] = ' ';
            if (fb->update_depth > 0) {
                fb_dirty_cell(fb->row, fb->col);
            } else {
                fb_draw_cell(fb->row, fb->col);
            }
        }
    } else {
        fb->cells[fb->row][fb->col] = c;
        if (fb->update_depth > 0) {
            fb_dirty_cell(fb->row, fb->col);
        } else {
            fb_draw_cell(fb->row, fb->col);
        }
        fb->col++;
        if (fb->col >= fb->cols) {
            fb->col = 0;
            fb->row++;
        }
    }

    if (fb->row >= fb->rows) {
        fb_scroll();
    }

    fb_draw_cursor_if_visible();
}

static void fb_begin_update(void) {
    if (!fb->ready) {
        return;
    }

    if (fb->update_depth == 0) {
        fb_erase_cursor();
    }
    fb->update_depth++;
}

static void fb_end_update(void) {
    if (!fb->ready || fb->update_depth <= 0) {
        return;
    }

    fb->update_depth--;
    if (fb->update_depth == 0) {
        fb_flush_dirty();
        fb_draw_cursor();
    }
}

static int fb_rows(void) {
    return fb->rows;
}

static int fb_cols(void) {
    return fb->cols;
}

static int fb_row(void) {
    return fb->row;
}

static int fb_col(void) {
    return fb->col;
}

static void fb_set_cursor(int row, int col) {
    if (!fb->ready) {
        return;
    }

    fb_erase_cursor_if_visible();

    if (row < 0) row = 0;
    if (row >= fb->rows) row = fb->rows - 1;
    if (col < 0) col = 0;
    if (col >= fb->cols) col = fb->cols - 1;

    fb->row = row;
    fb->col = col;
    fb_draw_cursor_if_visible();
}

static void fb_write_at(int row, int col, char c) {
    if (!fb->ready || row < 0 || col < 0 || row >= fb->rows || col >= fb->cols) {
        return;
    }

    fb_erase_cursor_if_visible();
    fb->cells[row][col] = c;
    fb_draw_cell(row, col);
    fb_draw_cursor_if_visible();
}

static const terminal_backend_t framebuffer_backend = {
    .clear = fb_clear,
    .putc = fb_putc,
    .begin_update = fb_begin_update,
    .end_update = fb_end_update,
    .rows = fb_rows,
    .cols = fb_cols,
    .row = fb_row,
    .col = fb_col,
    .set_cursor = fb_set_cursor,
    .write_at = fb_write_at,
};

int fb_console_init(void) {
    const boot_info_t* info = boot_info_get();
    u32 bytes;

#ifdef SMALLOS_FORCE_VGA_BACKEND
    terminal_puts("boot: PASS terminal: VGA text forced\n");
    return 0;
#endif

    if (!info->framebuffer_valid ||
        info->framebuffer_phys == 0u ||
        info->framebuffer_width < FB_FONT_WIDTH ||
        info->framebuffer_height < FB_FONT_HEIGHT ||
        info->framebuffer_bpp != 32u ||
        info->framebuffer_pitch < info->framebuffer_width * 4u) {
        return 0;
    }

    bytes = info->framebuffer_pitch * info->framebuffer_height;
    if (bytes == 0u || bytes > FB_MAX_BYTES) {
        return 0;
    }

    if (!fb) {
        fb = (fb_state_t*)kmalloc(sizeof(*fb));
        if (!fb) {
            return 0;
        }
    }

    k_memset(fb, 0, sizeof(*fb));
    fb->phys = info->framebuffer_phys;
    fb->width = info->framebuffer_width;
    fb->height = info->framebuffer_height;
    fb->pitch = info->framebuffer_pitch;
    fb->bpp = info->framebuffer_bpp;
    fb->cols = (int)(fb->width / FB_FONT_WIDTH);
    fb->rows = (int)(fb->height / FB_FONT_HEIGHT);
    if (fb->cols > FB_MAX_COLS) fb->cols = FB_MAX_COLS;
    if (fb->rows > FB_MAX_ROWS) fb->rows = FB_MAX_ROWS;
    if (fb->cols <= 0 || fb->rows <= 0) {
        return 0;
    }

    paging_map_kernel_range(FB_CONSOLE_VIRT_BASE,
                            fb->phys,
                            bytes,
                            PAGE_WRITE |
                            (cpu_write_combining_enabled()
                                ? PAGE_WRITE_COMBINE
                                : 0u));
    fb->base = (volatile u8*)FB_CONSOLE_VIRT_BASE;
    fb->ready = 1;
    fb_dirty_reset();
    if (cpu_write_combining_enabled()) {
        terminal_puts("boot: PASS framebuffer: write-combining enabled\n");
    } else {
        terminal_puts("boot: WARN framebuffer: write-combining unavailable\n");
    }

    terminal_set_backend(&framebuffer_backend);
    terminal_clear();
    return 1;
}

int fb_console_info(unsigned int* width, unsigned int* height,
                    unsigned int* pitch, unsigned int* bpp) {
    if (!fb || !fb->ready) {
        return 0;
    }
    if (width) *width = fb->width;
    if (height) *height = fb->height;
    if (pitch) *pitch = fb->pitch;
    if (bpp) *bpp = fb->bpp;
    return 1;
}

int fb_console_fill(unsigned int x, unsigned int y, unsigned int w,
                    unsigned int h, unsigned int color) {
    if (!fb || !fb->ready) {
        return 0;
    }
    if (x >= fb->width || y >= fb->height) {
        return 1;
    }
    if (w > fb->width - x) {
        w = fb->width - x;
    }
    if (h > fb->height - y) {
        h = fb->height - y;
    }

    for (unsigned int py = 0; py < h; py++) {
        volatile u32* row = (volatile u32*)(fb->base + (y + py) * fb->pitch + x * 4u);
        fb_store_words(row, color, w);
    }
    cpu_write_fence();
    return 1;
}

int fb_console_blit(unsigned int x, unsigned int y, unsigned int w,
                    unsigned int h, const unsigned int* pixels) {
    return fb_console_blit_stride(x, y, w, h, w, pixels);
}

int fb_console_blit_stride(unsigned int x, unsigned int y, unsigned int w,
                           unsigned int h, unsigned int pitch_pixels,
                           const unsigned int* pixels) {
    if (!fb || !fb->ready || !pixels) {
        return 0;
    }
    if (pitch_pixels < w) {
        return 0;
    }
    if (x >= fb->width || y >= fb->height) {
        return 1;
    }
    if (w > fb->width - x) {
        w = fb->width - x;
    }
    if (h > fb->height - y) {
        h = fb->height - y;
    }

    for (unsigned int py = 0; py < h; py++) {
        volatile u32* dst = (volatile u32*)(fb->base + (y + py) * fb->pitch + x * 4u);
        const unsigned int* src = pixels + py * pitch_pixels;
        fb_copy_words(dst, src, w);
    }
    cpu_write_fence();
    return 1;
}
