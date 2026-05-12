#include "user_lib.h"
#include "gfx.h"

#define COLOR_BG       0x00101518u
#define COLOR_PANEL    0x00192327u
#define COLOR_PANEL_2  0x00223034u
#define COLOR_TEXT     0x00EAF2E8u
#define COLOR_MUTED    0x0092A39Du
#define COLOR_USED     0x00E45D4Fu
#define COLOR_FREE     0x0047C27Au
#define COLOR_UNUSED   0x00141A1Du
#define COLOR_FRAME    0x0043554Eu
#define COLOR_GRID     0x002B383Cu

static unsigned int min_u(unsigned int a, unsigned int b) {
    return a < b ? a : b;
}

static unsigned int max_u(unsigned int a, unsigned int b) {
    return a > b ? a : b;
}

static unsigned int glyph_row(char ch, unsigned int row) {
    static const unsigned char digits[10][7] = {
        {14,17,19,21,25,17,14}, {4,12,4,4,4,4,14},
        {14,17,1,2,4,8,31},     {30,1,1,14,1,1,30},
        {2,6,10,18,31,2,2},     {31,16,16,30,1,1,30},
        {14,16,16,30,17,17,14}, {31,1,2,4,8,8,8},
        {14,17,17,14,17,17,14}, {14,17,17,15,1,1,14}
    };

    if (row >= 7) return 0;
    if (ch >= '0' && ch <= '9') return digits[ch - '0'][row];

    switch (ch) {
        case ' ': return 0;
        case '.': return row == 6 ? 4 : 0;
        case '%': {
            static const unsigned char g[7] = {17,2,4,8,16,17,0};
            return g[row];
        }
        case ':': return (row == 2 || row == 5) ? 4 : 0;
        case '/': {
            static const unsigned char g[7] = {1,2,2,4,8,8,16};
            return g[row];
        }
        case '-': return row == 3 ? 31 : 0;
        case 'A': { static const unsigned char g[7] = {14,17,17,31,17,17,17}; return g[row]; }
        case 'B': { static const unsigned char g[7] = {30,17,17,30,17,17,30}; return g[row]; }
        case 'C': { static const unsigned char g[7] = {14,17,16,16,16,17,14}; return g[row]; }
        case 'D': { static const unsigned char g[7] = {30,17,17,17,17,17,30}; return g[row]; }
        case 'E': { static const unsigned char g[7] = {31,16,16,30,16,16,31}; return g[row]; }
        case 'F': { static const unsigned char g[7] = {31,16,16,30,16,16,16}; return g[row]; }
        case 'G': { static const unsigned char g[7] = {14,17,16,23,17,17,14}; return g[row]; }
        case 'H': { static const unsigned char g[7] = {17,17,17,31,17,17,17}; return g[row]; }
        case 'I': { static const unsigned char g[7] = {14,4,4,4,4,4,14}; return g[row]; }
        case 'K': { static const unsigned char g[7] = {17,18,20,24,20,18,17}; return g[row]; }
        case 'L': { static const unsigned char g[7] = {16,16,16,16,16,16,31}; return g[row]; }
        case 'M': { static const unsigned char g[7] = {17,27,21,21,17,17,17}; return g[row]; }
        case 'N': { static const unsigned char g[7] = {17,25,21,19,17,17,17}; return g[row]; }
        case 'O': { static const unsigned char g[7] = {14,17,17,17,17,17,14}; return g[row]; }
        case 'P': { static const unsigned char g[7] = {30,17,17,30,16,16,16}; return g[row]; }
        case 'R': { static const unsigned char g[7] = {30,17,17,30,20,18,17}; return g[row]; }
        case 'S': { static const unsigned char g[7] = {15,16,16,14,1,1,30}; return g[row]; }
        case 'T': { static const unsigned char g[7] = {31,4,4,4,4,4,4}; return g[row]; }
        case 'U': { static const unsigned char g[7] = {17,17,17,17,17,17,14}; return g[row]; }
        case 'V': { static const unsigned char g[7] = {17,17,17,17,10,10,4}; return g[row]; }
        case 'Y': { static const unsigned char g[7] = {17,17,10,4,4,4,4}; return g[row]; }
        default: return 0;
    }
}

static void draw_char(gfx_surface_t* s, unsigned int x, unsigned int y,
                      char ch, unsigned int scale, unsigned int color) {
    if (ch >= 'a' && ch <= 'z') ch = (char)(ch - 'a' + 'A');

    for (unsigned int row = 0; row < 7; row++) {
        unsigned int bits = glyph_row(ch, row);
        for (unsigned int col = 0; col < 5; col++) {
            if (bits & (1u << (4u - col))) {
                gfx_fill_rect(s, x + col * scale, y + row * scale,
                              scale, scale, color);
            }
        }
    }
}

static void draw_text(gfx_surface_t* s, unsigned int x, unsigned int y,
                      const char* text, unsigned int scale, unsigned int color) {
    unsigned int cx = x;
    while (*text) {
        draw_char(s, cx, y, *text, scale, color);
        cx += 6u * scale;
        text++;
    }
}

static void append_char(char* buf, unsigned int* pos, unsigned int cap, char ch) {
    if (*pos + 1u < cap) {
        buf[*pos] = ch;
        (*pos)++;
        buf[*pos] = '\0';
    }
}

static void append_uint(char* buf, unsigned int* pos, unsigned int cap, unsigned int value) {
    char tmp[16];
    unsigned int n = 0;

    if (value == 0) {
        append_char(buf, pos, cap, '0');
        return;
    }

    while (value > 0 && n < sizeof(tmp)) {
        tmp[n++] = (char)('0' + value % 10u);
        value /= 10u;
    }
    while (n > 0) {
        append_char(buf, pos, cap, tmp[--n]);
    }
}

static void format_mb(char* buf, unsigned int cap, unsigned int bytes) {
    unsigned int pos = 0;
    unsigned int tenths = (bytes + 52428u) / 104858u;

    buf[0] = '\0';
    append_uint(buf, &pos, cap, tenths / 10u);
    append_char(buf, &pos, cap, '.');
    append_uint(buf, &pos, cap, tenths % 10u);
    append_char(buf, &pos, cap, ' ');
    append_char(buf, &pos, cap, 'M');
    append_char(buf, &pos, cap, 'B');
}

static void format_kb(char* buf, unsigned int cap, unsigned int bytes) {
    unsigned int pos = 0;

    buf[0] = '\0';
    append_uint(buf, &pos, cap, bytes / 1024u);
    append_char(buf, &pos, cap, ' ');
    append_char(buf, &pos, cap, 'K');
    append_char(buf, &pos, cap, 'B');
}

static void format_percent(char* buf, unsigned int cap, unsigned int part, unsigned int total) {
    unsigned int pos = 0;
    unsigned int pct10 = total ? (part * 1000u + total / 2u) / total : 0;

    buf[0] = '\0';
    append_uint(buf, &pos, cap, pct10 / 10u);
    append_char(buf, &pos, cap, '.');
    append_uint(buf, &pos, cap, pct10 % 10u);
    append_char(buf, &pos, cap, '%');
}

static void draw_card(gfx_surface_t* s, unsigned int x, unsigned int y,
                      unsigned int w, unsigned int h, unsigned int color) {
    gfx_fill_rect(s, x, y, w, h, color);
    gfx_fill_rect(s, x, y, w, 2, COLOR_FRAME);
    gfx_fill_rect(s, x, y + h - 2u, w, 2, COLOR_FRAME);
    gfx_fill_rect(s, x, y, 2, h, COLOR_FRAME);
    gfx_fill_rect(s, x + w - 2u, y, 2, h, COLOR_FRAME);
}

static unsigned int map_cell_size(unsigned int w, unsigned int h, unsigned int count) {
    for (unsigned int cell = 8u; cell > 1u; cell--) {
        unsigned int step = cell + 1u;
        unsigned int cols = w / step;
        unsigned int rows = h / step;
        if (cols > 0 && rows > 0 && cols * rows >= count) {
            return cell;
        }
    }
    return 1u;
}

static void draw_map(gfx_surface_t* s, const sys_fsinfo_t* info, const unsigned char* map) {
    unsigned int margin = max_u(16u, min_u(s->width, s->height) / 18u);
    unsigned int title_scale = s->width >= 560u ? 3u : 2u;
    unsigned int text_scale = s->width >= 560u ? 2u : 1u;
    unsigned int map_x = margin;
    unsigned int map_y = margin + 64u;
    unsigned int map_w = s->width - margin * 2u;
    unsigned int map_h = s->height - map_y - margin - 64u;
    unsigned int cell;
    unsigned int step;
    unsigned int cols;
    unsigned int rows;
    char tmp[32];

    gfx_clear(s, COLOR_BG);
    draw_text(s, margin, margin, "DISK SPACE MAP", title_scale, COLOR_TEXT);
    draw_text(s, margin, margin + 34u, "FAT16 DATA AREA", text_scale, COLOR_MUTED);

    draw_card(s, map_x, map_y, map_w, map_h, COLOR_PANEL);

    cell = map_cell_size(map_w - 4u, map_h - 4u, info->total_clusters);
    step = cell + (cell > 1u ? 1u : 0u);
    cols = (map_w - 4u) / step;
    rows = (map_h - 4u) / step;
    if (cols == 0) cols = 1;
    if (rows == 0) rows = 1;

    for (unsigned int y = 0; y < rows; y++) {
        for (unsigned int x = 0; x < cols; x++) {
            unsigned int idx = y * cols + x;
            unsigned int color = COLOR_UNUSED;
            if (idx < info->total_clusters) {
                color = map[idx] ? COLOR_USED : COLOR_FREE;
            }
            gfx_fill_rect(s,
                          map_x + 2u + x * step,
                          map_y + 2u + y * step,
                          cell,
                          cell,
                          color);
        }
    }

    gfx_fill_rect(s, margin, s->height - margin - 48u, 18u, 18u, COLOR_USED);
    draw_text(s, margin + 28u, s->height - margin - 48u, "USED", text_scale, COLOR_TEXT);
    gfx_fill_rect(s, margin + 130u, s->height - margin - 48u, 18u, 18u, COLOR_FREE);
    draw_text(s, margin + 158u, s->height - margin - 48u, "FREE", text_scale, COLOR_TEXT);

    draw_text(s, margin, s->height - margin - 22u, "1 CELL 4 SECTORS", text_scale, COLOR_MUTED);
    format_mb(tmp, sizeof(tmp), info->total_bytes);
    draw_text(s, margin + 250u, s->height - margin - 22u, "TOTAL", text_scale, COLOR_MUTED);
    draw_text(s, margin + 340u, s->height - margin - 22u, tmp, text_scale, COLOR_TEXT);
}

void _start(int argc, char** argv) {
    sys_fsinfo_t info;
    sys_fsmap_request_t map_req;
    unsigned char* map;
    gfx_context_t gfx;
    int rc;
    char ch;

    (void)argc;
    (void)argv;

    if (sys_fsinfo(&info) < 0) {
        u_puts("diskview: could not read filesystem usage\n");
        sys_exit(1);
    }

    map = (unsigned char*)malloc(info.total_clusters);
    if (!map) {
        u_puts("diskview: out of memory\n");
        sys_exit(1);
    }

    map_req.start_cluster = 0;
    map_req.max_clusters = info.total_clusters;
    map_req.states = map;
    map_req.out_clusters = 0;
    if (sys_fsmap(&map_req) < 0 || map_req.out_clusters != info.total_clusters) {
        free(map);
        u_puts("diskview: could not read filesystem map\n");
        sys_exit(1);
    }

    rc = gfx_open(&gfx);
    if (rc == -1) {
        free(map);
        u_puts("diskview: framebuffer display is not available\n");
        sys_exit(0);
    }
    if (rc < 0) {
        free(map);
        u_puts("diskview: could not acquire display\n");
        sys_exit(1);
    }

    draw_map(&gfx.backbuffer, &info, map);
    if (gfx_present(&gfx) < 0) {
        gfx_close(&gfx);
        free(map);
        u_puts("diskview: present failed\n");
        sys_exit(1);
    }

    sys_read_raw(&ch, 1u);
    gfx_close(&gfx);
    free(map);
    sys_exit(0);
}
