#include "fb_console.h"
#include "terminal.h"
#include "boot_info.h"
#include "paging.h"
#include "klib.h"

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
    char cells[FB_MAX_ROWS][FB_MAX_COLS];
} fb_state_t;

static fb_state_t fb;

static const u8* fb_font(void) {
    return (const u8*)BOOT_FONT_PHYS;
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

static void fb_put_pixel(u32 x, u32 y, u32 color) {
    if (!fb.ready || x >= fb.width || y >= fb.height) {
        return;
    }

    volatile u32* pixel = (volatile u32*)(fb.base + y * fb.pitch + x * 4u);
    *pixel = color;
}

static void fb_clear_rect(u32 x, u32 y, u32 w, u32 h) {
    for (u32 py = 0; py < h && y + py < fb.height; py++) {
        volatile u32* row = (volatile u32*)(fb.base + (y + py) * fb.pitch + x * 4u);
        u32 count = w;
        if (count > fb.width - x) count = fb.width - x;
        fb_store_words(row, FB_COLOR_BG, count);
    }
}

static void fb_draw_cell(int r, int c) {
    if (!fb.ready || r < 0 || c < 0 || r >= fb.rows || c >= fb.cols) {
        return;
    }

    unsigned char ch = (unsigned char)fb.cells[r][c];
    const u8* glyph = fb_font() + (u32)ch * FB_FONT_HEIGHT;
    u32 px0 = (u32)c * FB_FONT_WIDTH;
    u32 py0 = (u32)r * FB_FONT_HEIGHT;

    for (u32 py = 0; py < FB_FONT_HEIGHT; py++) {
        u8 bits = glyph[py];
        for (u32 px = 0; px < FB_FONT_WIDTH; px++) {
            u32 mask = 0x80u >> px;
            fb_put_pixel(px0 + px, py0 + py, (bits & mask) ? FB_COLOR_FG : FB_COLOR_BG);
        }
    }
}

static void fb_draw_cursor(void) {
    if (!fb.ready || fb.row < 0 || fb.col < 0 ||
        fb.row >= fb.rows || fb.col >= fb.cols) {
        return;
    }

    u32 px0 = (u32)fb.col * FB_FONT_WIDTH;
    u32 py0 = (u32)fb.row * FB_FONT_HEIGHT + FB_FONT_HEIGHT - 2u;

    for (u32 py = 0; py < 2u; py++) {
        for (u32 px = 0; px < FB_FONT_WIDTH; px++) {
            fb_put_pixel(px0 + px, py0 + py, FB_COLOR_FG);
        }
    }
}

static void fb_erase_cursor(void) {
    fb_draw_cell(fb.row, fb.col);
}

static void fb_scroll(void) {
    u32 row_bytes = fb.pitch * FB_FONT_HEIGHT;
    u32 copy_bytes = row_bytes * (u32)(fb.rows - 1);
    volatile u8* dst = fb.base;
    volatile u8* src = fb.base + row_bytes;

    for (u32 i = 0; i < copy_bytes; i++) {
        dst[i] = src[i];
    }

    for (int r = 1; r < fb.rows; r++) {
        for (int c = 0; c < fb.cols; c++) {
            fb.cells[r - 1][c] = fb.cells[r][c];
        }
    }

    for (int c = 0; c < fb.cols; c++) {
        fb.cells[fb.rows - 1][c] = ' ';
    }

    fb_clear_rect(0, (u32)(fb.rows - 1) * FB_FONT_HEIGHT,
                  fb.width, FB_FONT_HEIGHT);
    fb.row = fb.rows - 1;
    fb.col = 0;
}

static void fb_clear(void) {
    if (!fb.ready) {
        return;
    }

    fb_clear_rect(0, 0, fb.width, fb.height);
    for (int r = 0; r < fb.rows; r++) {
        for (int c = 0; c < fb.cols; c++) {
            fb.cells[r][c] = ' ';
        }
    }

    fb.row = 0;
    fb.col = 0;
    fb_draw_cursor();
}

static void fb_putc(char c) {
    if (!fb.ready) {
        return;
    }

    fb_erase_cursor();

    if (c == '\n') {
        fb.col = 0;
        fb.row++;
    } else if (c == '\r') {
        fb.col = 0;
    } else if (c == '\b') {
        if (fb.col > 0) {
            fb.col--;
            fb.cells[fb.row][fb.col] = ' ';
            fb_draw_cell(fb.row, fb.col);
        }
    } else {
        fb.cells[fb.row][fb.col] = c;
        fb_draw_cell(fb.row, fb.col);
        fb.col++;
        if (fb.col >= fb.cols) {
            fb.col = 0;
            fb.row++;
        }
    }

    if (fb.row >= fb.rows) {
        fb_scroll();
    }

    fb_draw_cursor();
}

static int fb_rows(void) {
    return fb.rows;
}

static int fb_cols(void) {
    return fb.cols;
}

static int fb_row(void) {
    return fb.row;
}

static int fb_col(void) {
    return fb.col;
}

static void fb_set_cursor(int row, int col) {
    if (!fb.ready) {
        return;
    }

    fb_erase_cursor();

    if (row < 0) row = 0;
    if (row >= fb.rows) row = fb.rows - 1;
    if (col < 0) col = 0;
    if (col >= fb.cols) col = fb.cols - 1;

    fb.row = row;
    fb.col = col;
    fb_draw_cursor();
}

static void fb_write_at(int row, int col, char c) {
    if (!fb.ready || row < 0 || col < 0 || row >= fb.rows || col >= fb.cols) {
        return;
    }

    fb_erase_cursor();
    fb.cells[row][col] = c;
    fb_draw_cell(row, col);
    fb_draw_cursor();
}

static const terminal_backend_t framebuffer_backend = {
    .clear = fb_clear,
    .putc = fb_putc,
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

    k_memset(&fb, 0, sizeof(fb));
    fb.phys = info->framebuffer_phys;
    fb.width = info->framebuffer_width;
    fb.height = info->framebuffer_height;
    fb.pitch = info->framebuffer_pitch;
    fb.bpp = info->framebuffer_bpp;
    fb.cols = (int)(fb.width / FB_FONT_WIDTH);
    fb.rows = (int)(fb.height / FB_FONT_HEIGHT);
    if (fb.cols > FB_MAX_COLS) fb.cols = FB_MAX_COLS;
    if (fb.rows > FB_MAX_ROWS) fb.rows = FB_MAX_ROWS;
    if (fb.cols <= 0 || fb.rows <= 0) {
        return 0;
    }

    paging_map_kernel_range(FB_CONSOLE_VIRT_BASE,
                            fb.phys,
                            bytes,
                            PAGE_WRITE);
    fb.base = (volatile u8*)FB_CONSOLE_VIRT_BASE;
    fb.ready = 1;

    terminal_set_backend(&framebuffer_backend);
    terminal_clear();
    return 1;
}

int fb_console_info(unsigned int* width, unsigned int* height,
                    unsigned int* pitch, unsigned int* bpp) {
    if (!fb.ready) {
        return 0;
    }
    if (width) *width = fb.width;
    if (height) *height = fb.height;
    if (pitch) *pitch = fb.pitch;
    if (bpp) *bpp = fb.bpp;
    return 1;
}

int fb_console_fill(unsigned int x, unsigned int y, unsigned int w,
                    unsigned int h, unsigned int color) {
    if (!fb.ready) {
        return 0;
    }
    if (x >= fb.width || y >= fb.height) {
        return 1;
    }
    if (w > fb.width - x) {
        w = fb.width - x;
    }
    if (h > fb.height - y) {
        h = fb.height - y;
    }

    for (unsigned int py = 0; py < h; py++) {
        volatile u32* row = (volatile u32*)(fb.base + (y + py) * fb.pitch + x * 4u);
        fb_store_words(row, color, w);
    }
    return 1;
}

int fb_console_blit(unsigned int x, unsigned int y, unsigned int w,
                    unsigned int h, const unsigned int* pixels) {
    unsigned int src_w = w;

    if (!fb.ready || !pixels) {
        return 0;
    }
    if (x >= fb.width || y >= fb.height) {
        return 1;
    }
    if (w > fb.width - x) {
        w = fb.width - x;
    }
    if (h > fb.height - y) {
        h = fb.height - y;
    }

    for (unsigned int py = 0; py < h; py++) {
        volatile u32* dst = (volatile u32*)(fb.base + (y + py) * fb.pitch + x * 4u);
        const unsigned int* src = pixels + py * src_w;
        fb_copy_words(dst, src, w);
    }
    return 1;
}
