#include "fat16.h"
#include "ata.h"
#include "../drivers/terminal.h"
#include "../kernel/klib.h"
#include "../kernel/memory.h"

/* ------------------------------------------------------------------ */
/* Volume geometry — must match mkfat16.c exactly                      */
/* ------------------------------------------------------------------ */

#define SECTOR_SIZE          512u
#define SECTORS_PER_CLUSTER  4u
#define CLUSTER_BYTES        (SECTORS_PER_CLUSTER * SECTOR_SIZE)   /* 2048 */
#define RESERVED_SECTORS     4u
#define NUM_FATS             2u
#define FAT_SECTORS          32u
#define ROOT_ENTRY_COUNT     512u
#define ROOT_DIR_SECTORS     (ROOT_ENTRY_COUNT * 32u / SECTOR_SIZE)  /* 32 */

/* Relative sector offsets within the FAT16 partition */
#define FAT1_REL_SECTOR   RESERVED_SECTORS                              /* 4   */
#define FAT2_REL_SECTOR   (FAT1_REL_SECTOR + FAT_SECTORS)               /* 36  */
#define ROOT_REL_SECTOR   (RESERVED_SECTORS + NUM_FATS * FAT_SECTORS)  /* 68  */
#define DATA_REL_SECTOR   (ROOT_REL_SECTOR + ROOT_DIR_SECTORS)         /* 100 */

/* MBR partition table layout in sector 0 */
#define MBR_PARTITION_TABLE_OFFSET   446u
#define MBR_PARTITION_ENTRY_SIZE      16u
#define MBR_PARTITION_TYPE_OFFSET     4u
#define MBR_PARTITION_LBA_OFFSET      8u
#define MBR_PARTITION_SIZE_OFFSET     12u
#define FAT16_PARTITION_ENTRY_INDEX   1u
#define FAT16_PARTITION_TYPE          0x06u

#define FIRST_CLUSTER     2u
#define FAT16_EOC_MIN     0xFFF8u
#define FAT_ENTRIES       (FAT_SECTORS * SECTOR_SIZE / 2u)
#define FAT16_MAX_WRITE_FILE_CLUSTERS (FAT16_MAX_WRITE_FILE_BYTES / CLUSTER_BYTES)

/* ------------------------------------------------------------------ */
/* Directory entry field offsets                                        */
/* ------------------------------------------------------------------ */

#define DIR_NAME          0    /* 11 bytes */
#define DIR_ATTR          11
#define DIR_FIRST_CLUSTER 26   /* u16 */
#define DIR_FILE_SIZE     28   /* u32 */

#define ATTR_VOLUME_ID    0x08
#define ATTR_DIRECTORY    0x10
#define ATTR_LONG_NAME    0x0Fu

/* ------------------------------------------------------------------ */
/* Module state                                                         */
/* ------------------------------------------------------------------ */

static int  s_initialised = 0;
static u32  s_fat16_lba   = 0;   /* absolute LBA of FAT16 partition start */

/* Scratch buffer — reused for BPB, FAT sector, directory sector reads */
static u8 s_sector[SECTOR_SIZE];

/* Writable working copies of the FAT and root directory. */
static u8 s_fat_buf[FAT_SECTORS * SECTOR_SIZE] __attribute__((aligned(2)));
static u8 s_root_buf[ROOT_DIR_SECTORS * SECTOR_SIZE] __attribute__((aligned(2)));
static u8 s_dir_buf[CLUSTER_BYTES] __attribute__((aligned(2)));
static u8 s_dir_buf2[CLUSTER_BYTES] __attribute__((aligned(2)));
static u32 s_write_chain[FAT16_MAX_WRITE_FILE_CLUSTERS];
static u32 s_old_write_chain[FAT16_MAX_WRITE_FILE_CLUSTERS];

/*
 * ELF load buffer — reused across fat16_load() calls.
 * Allocated from the kernel bump heap so large ELF loads cannot cross the
 * legacy VGA/BIOS hole at 0xA0000.  The caller (elf_run_image) copies all
 * data into PMM frames before returning, so the buffer is safe to reuse on
 * the next call.
 *
 * A separate static cluster scratch buffer is used to avoid a 2048-byte
 * allocation on the kernel stack inside hot paths that stream cluster data.
 */
static u8* s_load_buf = 0;

typedef struct {
    int is_root;
    u16 start_cluster;
    u32 size;
} dir_ctx_t;

typedef struct {
    const u8* data;
} mem_source_t;

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static u16 read_u16_le(const u8* buf, u32 off) {
    return (u16)(buf[off] | ((u16)buf[off + 1] << 8));
}

static u32 read_u32_le(const u8* buf, u32 off) {
    return (u32)buf[off]
         | ((u32)buf[off + 1] <<  8)
         | ((u32)buf[off + 2] << 16)
         | ((u32)buf[off + 3] << 24);
}

static u32 abs_lba(u32 rel) {
    return s_fat16_lba + rel;
}

static u32 cluster_to_rel_sector(u32 cluster) {
    return DATA_REL_SECTOR + (cluster - FIRST_CLUSTER) * SECTORS_PER_CLUSTER;
}

static int read_cluster_data(u32 cluster, u8* out) {
    u32 rel_sector = cluster_to_rel_sector(cluster);
    for (u32 s = 0; s < SECTORS_PER_CLUSTER; s++) {
        if (!ata_read_sectors(abs_lba(rel_sector + s), 1, out + s * SECTOR_SIZE)) {
            return 0;
        }
    }
    return 1;
}

static int write_cluster_data(u32 cluster, const u8* data) {
    u32 rel_sector = cluster_to_rel_sector(cluster);
    for (u32 s = 0; s < SECTORS_PER_CLUSTER; s++) {
        if (!ata_write_sectors(abs_lba(rel_sector + s), 1, data + s * SECTOR_SIZE)) {
            return 0;
        }
    }
    return 1;
}

static int is_sep(char c) {
    return c == '/' || c == '\\';
}

static int path_has_sep(const char* path) {
    if (!path) return 0;
    for (const char* p = path; *p; p++) {
        if (is_sep(*p)) return 1;
    }
    return 0;
}

static int path_next_component(const char** path, char* out, unsigned int out_size, int* is_last);

static int path_resolves_to_root(const char* path) {
    if (!path) return 1;

    int depth = 0;
    const char* cursor = path;
    char component[32];

    while (1) {
        int is_last = 0;
        int r = path_next_component(&cursor, component, sizeof(component), &is_last);
        if (r < 0) return 0;
        if (r == 0) break;

        if (k_strcmp(component, ".")) {
            continue;
        }

        if (k_strcmp(component, "..")) {
            if (depth > 0) depth--;
            continue;
        }

        depth++;
    }

    return depth == 0;
}

static int path_leaf_component(const char* path, char* out, unsigned int out_size) {
    if (!path || !out || out_size == 0) return 0;

    const char* cursor = path;
    char component[32];
    int saw_component = 0;

    while (1) {
        int is_last = 0;
        int r = path_next_component(&cursor, component, sizeof(component), &is_last);
        if (r < 0) return 0;
        if (r == 0) break;

        if (k_strcmp(component, ".") || k_strcmp(component, "..")) {
            continue;
        }

        if (k_strlen(component) + 1u > out_size) {
            return 0;
        }
        saw_component = 1;
        k_memcpy(out, component, (k_size_t)k_strlen(component) + 1u);
    }

    return saw_component;
}

static dir_ctx_t root_dir_ctx(void) {
    dir_ctx_t dir;
    dir.is_root = 1;
    dir.start_cluster = 0;
    dir.size = ROOT_DIR_SECTORS * SECTOR_SIZE;
    return dir;
}

static dir_ctx_t dir_ctx_from_entry(const u8 entry[32]) {
    dir_ctx_t dir;
    dir.is_root = 0;
    dir.start_cluster = read_u16_le(entry, DIR_FIRST_CLUSTER);
    dir.size = read_u32_le(entry, DIR_FILE_SIZE);
    return dir;
}

/*
 * fat_entry(cluster)
 *
 * Read the FAT16 entry for `cluster` and return its value.
 * Returns FAT16_EOC_MIN on read error so callers terminate the chain.
 */
static u16 fat_entry(u32 cluster) {
    u32 entries_per_sector = SECTOR_SIZE / 2u;
    u32 fat_sector_rel     = FAT1_REL_SECTOR + cluster / entries_per_sector;
    u32 offset_in_sector   = (cluster % entries_per_sector) * 2u;

    if (!ata_read_sectors(abs_lba(fat_sector_rel), 1, s_sector)) {
        terminal_puts("fat16: FAT read error\n");
        return FAT16_EOC_MIN;
    }
    return read_u16_le(s_sector, offset_in_sector);
}

static void write_u16_le(u8* buf, u32 off, u16 val) {
    buf[off] = (u8)(val & 0xFFu);
    buf[off + 1] = (u8)((val >> 8) & 0xFFu);
}

static void write_u32_le(u8* buf, u32 off, u32 val) {
    buf[off] = (u8)(val & 0xFFu);
    buf[off + 1] = (u8)((val >> 8) & 0xFFu);
    buf[off + 2] = (u8)((val >> 16) & 0xFFu);
    buf[off + 3] = (u8)((val >> 24) & 0xFFu);
}

/* ------------------------------------------------------------------ */
/* 8.3 name matching                                                   */
/* ------------------------------------------------------------------ */

/* strlen — no libc */
static unsigned int fat16_strlen(const char* s) {
    unsigned int n = 0;
    while (s[n]) n++;
    return n;
}

static char to_upper(char c) {
    if (c >= 'a' && c <= 'z') return (char)(c - 32);
    return c;
}

/*
 * to_83(path, name83)
 *
 * Convert a human-readable filename into uppercase 8.3 form.  Path
 * separators are ignored; only the last component is used.
 */
static void to_83(const char* path, u8 name83[11]) {
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    const char* dot = 0;
    for (const char* p = base; *p; p++) {
        if (*p == '.') dot = p;
    }

    for (int i = 0; i < 11; i++) {
        name83[i] = ' ';
    }

    const char* end_base = dot ? dot : base + fat16_strlen(base);
    int ni = 0;
    for (const char* p = base; p < end_base && ni < 8; p++, ni++) {
        name83[ni] = (u8)to_upper(*p);
    }

    if (dot) {
        int ei = 0;
        for (const char* p = dot + 1; *p && ei < 3; p++, ei++) {
            name83[8 + ei] = (u8)to_upper(*p);
        }
    }
}

static int match_83(const u8 dir_name[11], const char* name);

static int path_next_component(const char** path, char* out, unsigned int out_size, int* is_last) {
    const char* p = *path;

    while (*p && is_sep(*p)) p++;
    if (*p == '\0') {
        *path = p;
        return 0;
    }

    unsigned int len = 0;
    while (p[len] && !is_sep(p[len])) len++;
    if (len == 0 || len >= out_size) {
        return -1;
    }

    for (unsigned int i = 0; i < len; i++) {
        out[i] = p[i];
    }
    out[len] = '\0';

    p += len;
    while (*p && is_sep(*p)) p++;
    *is_last = (*p == '\0');
    *path = p;
    return 1;
}

static int entry_is_valid(const u8* entry) {
    if (entry[DIR_NAME] == 0x00) return 0;
    if (entry[DIR_NAME] == 0xE5) return 0;
    if (entry[DIR_NAME] == '.' &&
        (entry[DIR_NAME + 1] == 0x00 ||
         entry[DIR_NAME + 1] == ' ' ||
         entry[DIR_NAME + 1] == '.')) return 0;
    if (entry[DIR_ATTR] == ATTR_LONG_NAME) return 0;
    if (entry[DIR_ATTR] & ATTR_VOLUME_ID) return 0;
    return 1;
}

static int entry_is_dir(const u8* entry) {
    return (entry[DIR_ATTR] & ATTR_DIRECTORY) != 0;
}

static void write_dot_entry(u8* entry, u16 cluster, u32 size, int is_dotdot) {
    k_memset(entry, 0, 32);
    entry[0] = '.';
    if (is_dotdot) {
        entry[1] = '.';
    }
    entry[DIR_ATTR] = ATTR_DIRECTORY;
    write_u16_le(entry, DIR_FIRST_CLUSTER, cluster);
    write_u32_le(entry, DIR_FILE_SIZE, size);
}

static int dir_scan(const dir_ctx_t* dir,
                    int (*cb)(const u8* entry, void* ctx),
                    void* ctx);

static unsigned int decimal_len(u32 value) {
    unsigned int len = 1;
    while (value >= 10u) {
        value /= 10u;
        len++;
    }
    return len;
}

static unsigned int display_name_width(const u8 name83[11], int is_dir) {
    unsigned int width = 0;
    int base_end = 8;
    while (base_end > 0 && name83[base_end - 1] == ' ') base_end--;

    int ext_end = 3;
    while (ext_end > 0 && name83[8 + ext_end - 1] == ' ') ext_end--;

    for (int i = 0; i < base_end; i++) {
        width++;
    }

    if (ext_end > 0) {
        width++;
        for (int i = 0; i < ext_end; i++) {
            width++;
        }
    }

    if (is_dir) {
        width++;
    }

    return width;
}

static unsigned int display_size_width(u32 size) {
    if (size < 1024u) {
        return decimal_len(size) + 2u;
    }

    return decimal_len(size / 1024u) + 3u;
}

static unsigned int entry_display_name(const u8 name83[11], int is_dir, char* out, unsigned int out_size) {
    unsigned int pos = 0;
    int base_end = 8;
    while (base_end > 0 && name83[base_end - 1] == ' ') base_end--;

    int ext_end = 3;
    while (ext_end > 0 && name83[8 + ext_end - 1] == ' ') ext_end--;

    if (out_size == 0) {
        return 0;
    }

    for (int i = 0; i < base_end && pos + 1u < out_size; i++) {
        out[pos++] = (char)name83[i];
    }

    if (ext_end > 0 && pos + 1u < out_size) {
        out[pos++] = '.';
        for (int i = 0; i < ext_end && pos + 1u < out_size; i++) {
            out[pos++] = (char)name83[8 + i];
        }
    }

    if (is_dir && pos + 1u < out_size) {
        out[pos++] = '/';
    }

    out[pos] = '\0';
    return pos;
}

static int wildcard_match(const char* pattern, const char* text) {
    if (*pattern == '*') {
        while (*pattern == '*') {
            pattern++;
        }

        if (*pattern == '\0') {
            return 1;
        }

        while (*text) {
            if (wildcard_match(pattern, text)) {
                return 1;
            }
            text++;
        }

        return 0;
    }

    if (*pattern == '?') {
        if (*text == '\0') {
            return 0;
        }
        return wildcard_match(pattern + 1, text + 1);
    }

    if (*pattern == '\0') {
        return *text == '\0';
    }

    if (*text == '\0') {
        return 0;
    }

    if (to_upper(*pattern) != to_upper(*text)) {
        return 0;
    }

    return wildcard_match(pattern + 1, text + 1);
}

static int entry_matches_pattern(const u8* entry, const char* pattern) {
    if (!pattern || pattern[0] == '\0') {
        return 1;
    }

    char name[32];
    entry_display_name(entry + DIR_NAME, 0, name, sizeof(name));
    return wildcard_match(pattern, name);
}

static void print_name_field(const u8 name83[11], int is_dir, unsigned int width) {
    unsigned int printed = 0;
    int base_end = 8;
    while (base_end > 0 && name83[base_end - 1] == ' ') base_end--;

    int ext_end = 3;
    while (ext_end > 0 && name83[8 + ext_end - 1] == ' ') ext_end--;

    for (int i = 0; i < base_end; i++) {
        terminal_putc((char)name83[i]);
        printed++;
    }

    if (ext_end > 0) {
        terminal_putc('.');
        printed++;
        for (int i = 0; i < ext_end; i++) {
            terminal_putc((char)name83[8 + i]);
            printed++;
        }
    }

    if (is_dir) {
        terminal_putc('/');
        printed++;
    }

    while (printed < width) {
        terminal_putc(' ');
        printed++;
    }
}

static void print_right_aligned_uint(u32 value, unsigned int width) {
    unsigned int len = decimal_len(value);
    while (len < width) {
        terminal_putc(' ');
        len++;
    }
    terminal_put_uint(value);
}

static void print_size_field(u32 size, int is_dir, unsigned int width) {
    if (is_dir) {
        print_right_aligned_uint(0, width - 1u);
        terminal_putc('-');
        return;
    }

    u32 value;
    const char* unit;

    if (size < 1024u) {
        value = size;
        unit = " B";
    } else {
        value = size / 1024u;
        unit = " KB";
    }

    print_right_aligned_uint(value, width - (size < 1024u ? 2u : 3u));
    terminal_puts(unit);
}

static int entry_sort_cmp(const u8* a, const u8* b) {
    int a_is_dir = entry_is_dir(a);
    int b_is_dir = entry_is_dir(b);

    if (a_is_dir != b_is_dir) {
        return a_is_dir ? -1 : 1;
    }

    for (int i = 0; i < 11; i++) {
        if (a[DIR_NAME + i] != b[DIR_NAME + i]) {
            return (int)a[DIR_NAME + i] - (int)b[DIR_NAME + i];
        }
    }

    return 0;
}

typedef struct {
    int want_dirs;
    int have_last;
    u8 last[32];
    int found;
    u8 best[32];
    const char* pattern;
} list_sorted_ctx_t;

typedef struct {
    unsigned int name_width;
    unsigned int size_width;
    unsigned int cluster_width;
    const char* pattern;
} list_widths_t;

static int list_sorted_cb(const u8* entry, void* raw) {
    list_sorted_ctx_t* ctx = (list_sorted_ctx_t*)raw;

    if (entry_is_dir(entry) != ctx->want_dirs) {
        return 1;
    }

    if (!entry_matches_pattern(entry, ctx->pattern)) {
        return 1;
    }

    if (ctx->have_last && entry_sort_cmp(entry, ctx->last) <= 0) {
        return 1;
    }

    if (!ctx->found || entry_sort_cmp(entry, ctx->best) < 0) {
        k_memcpy(ctx->best, entry, 32);
        ctx->found = 1;
    }

    return 1;
}

static int list_widths_cb(const u8* entry, void* raw) {
    list_widths_t* widths = (list_widths_t*)raw;
    if (!entry_matches_pattern(entry, widths->pattern)) {
        return 1;
    }

    unsigned int name_width = display_name_width(entry + DIR_NAME, entry_is_dir(entry));
    unsigned int size_width = display_size_width(read_u32_le(entry, DIR_FILE_SIZE));
    unsigned int cluster_width = decimal_len(read_u16_le(entry, DIR_FIRST_CLUSTER));

    if (name_width > widths->name_width) {
        widths->name_width = name_width;
    }
    if (size_width > widths->size_width) {
        widths->size_width = size_width;
    }
    if (cluster_width > widths->cluster_width) {
        widths->cluster_width = cluster_width;
    }

    return 1;
}

static list_widths_t dir_list_widths(const dir_ctx_t* dir, const char* pattern) {
    list_widths_t widths;
    widths.name_width = 4u;
    widths.size_width = 4u;
    widths.cluster_width = 7u;
    widths.pattern = pattern;
    dir_scan(dir, list_widths_cb, &widths);
    return widths;
}

static void print_list_row(const u8* entry, const list_widths_t* widths) {
    terminal_puts("  ");
    int is_dir = entry_is_dir(entry);
    print_name_field(entry + DIR_NAME, is_dir, widths->name_width);
    terminal_puts("  ");
    print_size_field(read_u32_le(entry, DIR_FILE_SIZE), is_dir, widths->size_width);
    terminal_puts("  ");
    print_right_aligned_uint(read_u16_le(entry, DIR_FIRST_CLUSTER), widths->cluster_width);
    terminal_putc('\n');
}

static void list_sorted_group(const dir_ctx_t* dir, int want_dirs, const list_widths_t* widths, const char* pattern) {
    list_sorted_ctx_t ctx;
    k_memset(&ctx, 0, sizeof(ctx));
    ctx.want_dirs = want_dirs;
    ctx.pattern = pattern;

    for (;;) {
        ctx.found = 0;
        if (!dir_scan(dir, list_sorted_cb, &ctx)) {
            return;
        }

        if (!ctx.found) {
            return;
        }

        print_list_row(ctx.best, widths);

        k_memcpy(ctx.last, ctx.best, 32);
        ctx.have_last = 1;
    }
}

static int dir_scan(const dir_ctx_t* dir,
                    int (*cb)(const u8* entry, void* ctx),
                    void* ctx)
{
    if (!dir || !cb) return 0;
    u8 sector_buf[SECTOR_SIZE];

    if (dir->is_root) {
        for (u32 s = 0; s < ROOT_DIR_SECTORS; s++) {
            if (!ata_read_sectors(abs_lba(ROOT_REL_SECTOR + s), 1, s_sector)) {
                terminal_puts("fat16: root dir read error\n");
                return 0;
            }

            for (u32 e = 0; e < SECTOR_SIZE / 32u; e++) {
                const u8* entry = s_sector + e * 32u;
                if (!entry_is_valid(entry)) {
                    if (entry[DIR_NAME] == 0x00) return 1;
                    continue;
                }
                if (!cb(entry, ctx)) return 1;
            }
        }
        return 1;
    }

    if (dir->start_cluster < FIRST_CLUSTER || dir->size == 0) {
        return 1;
    }

    u32 bytes_remaining = dir->size;
    u32 cluster = dir->start_cluster;

    while (cluster >= FIRST_CLUSTER && cluster < FAT16_EOC_MIN && bytes_remaining > 0) {
        u32 rel_sector = cluster_to_rel_sector(cluster);
        for (u32 s = 0; s < SECTORS_PER_CLUSTER && bytes_remaining > 0; s++) {
            if (!ata_read_sectors(abs_lba(rel_sector + s), 1, sector_buf)) {
                terminal_puts("fat16: directory read error\n");
                return 0;
            }

            u32 bytes_in_sector = bytes_remaining < SECTOR_SIZE
                                ? bytes_remaining
                                : SECTOR_SIZE;

            for (u32 off = 0; off + 32u <= bytes_in_sector; off += 32u) {
                const u8* entry = sector_buf + off;
                if (!entry_is_valid(entry)) {
                    if (entry[DIR_NAME] == 0x00) return 1;
                    continue;
                }
                if (!cb(entry, ctx)) return 1;
            }

            bytes_remaining -= bytes_in_sector;
            if (bytes_remaining == 0) break;
        }

        cluster = fat_entry(cluster);
    }

    return 1;
}

typedef struct {
    const char* name;
    u8* out_entry;
    int found;
} find_entry_ctx_t;

static int find_entry_cb(const u8* entry, void* raw) {
    find_entry_ctx_t* ctx = (find_entry_ctx_t*)raw;
    if (match_83(entry + DIR_NAME, ctx->name)) {
        k_memcpy(ctx->out_entry, entry, 32);
        ctx->found = 1;
        return 0;
    }
    return 1;
}

static int dir_find_entry(const dir_ctx_t* dir, const char* name, u8 out_entry[32]) {
    find_entry_ctx_t ctx;
    ctx.name = name;
    ctx.out_entry = out_entry;
    ctx.found = 0;

    if (!dir_scan(dir, find_entry_cb, &ctx)) {
        return 0;
    }

    return ctx.found;
}

typedef struct {
    int count;
} count_entries_ctx_t;

static int count_entry_cb(const u8* entry, void* raw) {
    (void)entry;
    count_entries_ctx_t* ctx = (count_entries_ctx_t*)raw;
    ctx->count++;
    return 1;
}

static int dir_entry_count(const dir_ctx_t* dir) {
    count_entries_ctx_t ctx;
    ctx.count = 0;
    if (!dir_scan(dir, count_entry_cb, &ctx)) {
        return -1;
    }
    return ctx.count;
}

typedef struct {
    dir_ctx_t parent;
    u8 entry[32];
    int has_entry;
} resolved_path_t;

static int resolve_path(const char* path, resolved_path_t* out) {
    if (!out) return 0;

    dir_ctx_t stack[16];
    int depth = 0;
    stack[0] = root_dir_ctx();

    if (!path || path[0] == '\0') {
        out->parent = stack[0];
        out->has_entry = 0;
        return 1;
    }

    const char* cursor = path;
    char component[32];

    while (1) {
        int is_last = 0;
        int r = path_next_component(&cursor, component, sizeof(component), &is_last);
        if (r < 0) return 0;
        if (r == 0) break;

        if (k_strcmp(component, ".")) {
            if (is_last) {
                out->parent = stack[depth];
                out->has_entry = 0;
                return 1;
            }
            continue;
        }

        if (k_strcmp(component, "..")) {
            if (depth > 0) depth--;
            if (is_last) {
                out->parent = stack[depth];
                out->has_entry = 0;
                return 1;
            }
            continue;
        }

        if (is_last) {
            if (!dir_find_entry(&stack[depth], component, out->entry)) {
                return 0;
            }
            out->parent = stack[depth];
            out->has_entry = 1;
            return 1;
        }

        if (!dir_find_entry(&stack[depth], component, out->entry)) {
            return 0;
        }
        if (!entry_is_dir(out->entry)) {
            return 0;
        }
        if (depth + 1 >= (int)(sizeof(stack) / sizeof(stack[0]))) {
            return 0;
        }
        stack[++depth] = dir_ctx_from_entry(out->entry);
    }

    out->parent = stack[depth];
    out->has_entry = 0;
    return 1;
}

typedef struct {
    u32 target;
    u32 seen;
    char* out_name;
    u32 out_name_size;
    u32* out_size;
    int* out_is_dir;
    int found;
} dirent_at_ctx_t;

static int dirent_at_cb(const u8* entry, void* raw) {
    dirent_at_ctx_t* ctx = (dirent_at_ctx_t*)raw;
    char name[256];

    if (ctx->seen != ctx->target) {
        ctx->seen++;
        return 1;
    }

    entry_display_name(entry + DIR_NAME, entry_is_dir(entry), name, sizeof(name));
    if (k_strlen(name) + 1u > ctx->out_name_size) {
        return 0;
    }
    k_memcpy(ctx->out_name, name, k_strlen(name) + 1u);
    if (ctx->out_size) {
        *ctx->out_size = read_u32_le(entry, DIR_FILE_SIZE);
    }
    if (ctx->out_is_dir) {
        *ctx->out_is_dir = entry_is_dir(entry) ? 1 : 0;
    }
    ctx->found = 1;
    return 0;
}

int fat16_dirent_at(const char* path,
                    u32 index,
                    char* out_name,
                    u32 out_name_size,
                    u32* out_size,
                    int* out_is_dir) {
    resolved_path_t resolved;
    dirent_at_ctx_t ctx;

    if (!s_initialised || !out_name || out_name_size == 0u) {
        return 0;
    }

    if (!resolve_path(path, &resolved)) {
        return 0;
    }

    if (resolved.has_entry && !entry_is_dir(resolved.entry)) {
        return 0;
    }

    ctx.target = index;
    ctx.seen = 0u;
    ctx.out_name = out_name;
    ctx.out_name_size = out_name_size;
    ctx.out_size = out_size;
    ctx.out_is_dir = out_is_dir;
    ctx.found = 0;

    if (resolved.has_entry) {
        dir_ctx_t dir = dir_ctx_from_entry(resolved.entry);
        dir_scan(&dir, dirent_at_cb, &ctx);
    } else {
        dir_scan(&resolved.parent, dirent_at_cb, &ctx);
    }

    return ctx.found;
}

typedef struct {
    dir_ctx_t parent;
    char leaf[32];
} create_path_t;

static int resolve_create_path(const char* path, create_path_t* out) {
    if (!out || !path || path[0] == '\0') return 0;

    dir_ctx_t stack[16];
    int depth = 0;
    stack[0] = root_dir_ctx();

    const char* cursor = path;
    char component[32];
    u8 entry[32];

    while (1) {
        int is_last = 0;
        int r = path_next_component(&cursor, component, sizeof(component), &is_last);
        if (r < 0) return 0;
        if (r == 0) return 0;

        if (k_strcmp(component, ".")) {
            if (is_last) return 0;
            continue;
        }

        if (k_strcmp(component, "..")) {
            if (depth > 0) depth--;
            if (is_last) return 0;
            continue;
        }

        if (is_last) {
            out->parent = stack[depth];
            k_memcpy(out->leaf, component, (k_size_t)k_strlen(component) + 1u);
            return 1;
        }

        if (!dir_find_entry(&stack[depth], component, entry)) {
            return 0;
        }
        if (!entry_is_dir(entry)) {
            return 0;
        }
        if (depth + 1 >= (int)(sizeof(stack) / sizeof(stack[0]))) {
            return 0;
        }
        stack[++depth] = dir_ctx_from_entry(entry);
    }
}

static int dir_read_ctx(const dir_ctx_t* dir, u8* buf, u32 buf_size, u32* out_size) {
    if (!dir || !buf) return 0;
    u8 sector_buf[SECTOR_SIZE];

    if (dir->is_root) {
        u32 root_bytes = ROOT_DIR_SECTORS * SECTOR_SIZE;
        if (buf_size < root_bytes) return 0;
        k_memcpy(buf, s_root_buf, root_bytes);
        if (out_size) *out_size = root_bytes;
        return 1;
    }

    if (dir->size > buf_size) return 0;
    if (dir->size == 0) {
        if (out_size) *out_size = 0;
        return 1;
    }

    u32 remaining = dir->size;
    u32 offset = 0;
    u32 cluster = dir->start_cluster;

    while (remaining > 0) {
        if (cluster < FIRST_CLUSTER || cluster >= FAT16_EOC_MIN) {
            return 0;
        }

        u32 rel_sector = cluster_to_rel_sector(cluster);
        for (u32 s = 0; s < SECTORS_PER_CLUSTER && remaining > 0; s++) {
            if (!ata_read_sectors(abs_lba(rel_sector + s), 1, sector_buf)) {
                terminal_puts("fat16: directory read error\n");
                return 0;
            }

            u32 to_copy = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
            k_memcpy(buf + offset, sector_buf, to_copy);
            offset += to_copy;
            remaining -= to_copy;
        }

        if (remaining == 0) break;
        cluster = fat_entry(cluster);
    }

    if (out_size) *out_size = offset;
    return 1;
}

static int dir_write_ctx(const dir_ctx_t* dir, const u8* buf, u32 size) {
    if (!dir || !buf) return 0;
    u8 sector_buf[SECTOR_SIZE];

    if (dir->is_root) {
        u32 root_bytes = ROOT_DIR_SECTORS * SECTOR_SIZE;
        if (size != root_bytes) return 0;
        k_memcpy(s_root_buf, buf, root_bytes);
        return 1;
    }

    if (size != dir->size) return 0;
    if (size == 0) return 1;

    u32 remaining = size;
    u32 offset = 0;
    u32 cluster = dir->start_cluster;

    while (remaining > 0) {
        if (cluster < FIRST_CLUSTER || cluster >= FAT16_EOC_MIN) {
            return 0;
        }

        u32 rel_sector = cluster_to_rel_sector(cluster);
        for (u32 s = 0; s < SECTORS_PER_CLUSTER && remaining > 0; s++) {
            u32 chunk = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
            k_memset(sector_buf, 0, SECTOR_SIZE);
            k_memcpy(sector_buf, buf + offset, chunk);

            if (!ata_write_sectors(abs_lba(rel_sector + s), 1, sector_buf)) {
                terminal_puts("fat16: directory write error\n");
                return 0;
            }

            offset += chunk;
            remaining -= chunk;
        }
        if (remaining == 0) break;
        cluster = fat_entry(cluster);
    }

    return 1;
}

static int dir_find_slot_in_buf(const u8* buf, u32 size, const char* name, int* found_slot, int* free_slot) {
    if (!buf || !name || !found_slot || !free_slot) return 0;

    *found_slot = -1;
    *free_slot = -1;

    u32 entries = size / 32u;
    for (u32 i = 0; i < entries; i++) {
        const u8* entry = buf + i * 32u;

        if (entry[DIR_NAME] == 0x00) {
            if (*free_slot < 0) *free_slot = (int)i;
            break;
        }
        if (entry[DIR_NAME] == 0xE5) {
            if (*free_slot < 0) *free_slot = (int)i;
            continue;
        }
        if (!entry_is_valid(entry)) {
            continue;
        }
        if (match_83(entry + DIR_NAME, name)) {
            *found_slot = (int)i;
            return 1;
        }
    }

    return 1;
}

static int dir_buf_has_entries(const u8* buf, u32 size) {
    if (!buf) return 0;

    u32 entries = size / 32u;
    for (u32 i = 0; i < entries; i++) {
        const u8* entry = buf + i * 32u;
        if (entry[DIR_NAME] == 0x00) {
            return 0;
        }
        if (!entry_is_valid(entry)) {
            continue;
        }
        if (entry[DIR_NAME] == '.' &&
            (entry[DIR_NAME + 1] == ' ' || entry[DIR_NAME + 1] == '.' || entry[DIR_NAME + 1] == 0x00)) {
            continue;
        }
        if (entry[DIR_NAME] == '.' && entry[DIR_NAME + 1] == '.') {
            continue;
        }
        if (entry[DIR_NAME] != '.' && entry[DIR_NAME] != 0xE5) {
            return 1;
        }
    }

    return 0;
}

static void free_cluster_chain(u16* fat, u16 start_cluster) {
    u16 cluster = start_cluster;

    while (cluster >= FIRST_CLUSTER && cluster < FAT16_EOC_MIN) {
        u16 next = fat[cluster];
        fat[cluster] = 0;
        if (next >= FAT16_EOC_MIN) {
            break;
        }
        cluster = next;
    }
}

static int path_leaf_to_83(const char* leaf, u8 name83[11]) {
    if (!leaf || !name83 || leaf[0] == '\0') {
        return 0;
    }

    if (k_strcmp(leaf, ".") || k_strcmp(leaf, "..")) {
        return 0;
    }

    to_83(leaf, name83);
    return name83[0] != ' ';
}

static int dir_list_ctx(const dir_ctx_t* dir, const char* label, const char* pattern) {
    if (label && k_strcmp(label, "root directory")) {
        terminal_puts("fat16 root directory:\n");
    } else {
        terminal_puts("fat16 directory: ");
        terminal_puts(label ? label : "");
        terminal_putc('\n');
    }

    list_widths_t widths = dir_list_widths(dir, pattern);

    terminal_puts("  name");
    for (unsigned int i = 4u; i < widths.name_width; i++) {
        terminal_putc(' ');
    }
    terminal_puts("  size");
    for (unsigned int i = 4u; i < widths.size_width; i++) {
        terminal_putc(' ');
    }
    terminal_puts("  cluster\n");

    list_sorted_group(dir, 1, &widths, pattern);
    list_sorted_group(dir, 0, &widths, pattern);
    return 1;
}

static void write_dirent(u8* entry, const u8 name83[11], u16 start_cluster, u32 file_size) {
    k_memset(entry, 0, 32);
    k_memcpy(entry, name83, 11);
    entry[11] = 0x20;
    write_u16_le(entry, DIR_FIRST_CLUSTER, start_cluster);
    write_u32_le(entry, DIR_FILE_SIZE, file_size);
}

static int load_fat_and_root(void) {
    for (u32 s = 0; s < FAT_SECTORS; s++) {
        if (!ata_read_sectors(abs_lba(FAT1_REL_SECTOR + s), 1, s_fat_buf + s * SECTOR_SIZE)) {
            terminal_puts("fat16: FAT read error\n");
            return 0;
        }
    }

    for (u32 s = 0; s < ROOT_DIR_SECTORS; s++) {
        if (!ata_read_sectors(abs_lba(ROOT_REL_SECTOR + s), 1, s_root_buf + s * SECTOR_SIZE)) {
            terminal_puts("fat16: root dir read error\n");
            return 0;
        }
    }

    return 1;
}

static int write_fat_and_root(void) {
    for (u32 s = 0; s < FAT_SECTORS; s++) {
        if (!ata_write_sectors(abs_lba(FAT1_REL_SECTOR + s), 1, s_fat_buf + s * SECTOR_SIZE)) {
            terminal_puts("fat16: FAT write error\n");
            return 0;
        }
    }

    for (u32 s = 0; s < FAT_SECTORS; s++) {
        if (!ata_write_sectors(abs_lba(FAT2_REL_SECTOR + s), 1, s_fat_buf + s * SECTOR_SIZE)) {
            terminal_puts("fat16: FAT mirror write error\n");
            return 0;
        }
    }

    for (u32 s = 0; s < ROOT_DIR_SECTORS; s++) {
        if (!ata_write_sectors(abs_lba(ROOT_REL_SECTOR + s), 1, s_root_buf + s * SECTOR_SIZE)) {
            terminal_puts("fat16: root dir write error\n");
            return 0;
        }
    }
    return 1;
}

static int find_free_chain(const u16* fat, u32 clusters_needed, u32* chain) {
    u32 found = 0;
    for (u32 c = FIRST_CLUSTER; c < (u32)FAT_ENTRIES && found < clusters_needed; c++) {
        if (fat[c] == 0) {
            chain[found++] = c;
        }
    }
    return found == clusters_needed;
}

static int mem_source_read(void* ctx, u32 offset, u8* out, u32 len) {
    mem_source_t* mem = (mem_source_t*)ctx;
    if (!mem || (!mem->data && len > 0) || !out) {
        return 0;
    }
    if (len > 0) {
        k_memcpy(out, mem->data + offset, len);
    }
    return 1;
}

static int write_data_clusters(const u32* chain,
                               u32 clusters,
                               const fat16_data_source_t* source,
                               u32 size) {
    u8 sector[SECTOR_SIZE];
    u32 offset = 0;
    u32 remaining = size;

    if (!source || !source->read) {
        return 0;
    }

    for (u32 i = 0; i < clusters; i++) {
        for (u32 s = 0; s < SECTORS_PER_CLUSTER; s++) {
            k_memset(sector, 0, SECTOR_SIZE);
            u32 chunk = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
            if (chunk > 0) {
                if (!source->read(source->ctx, offset, sector, chunk)) {
                    terminal_puts("fat16: data source read error\n");
                    return 0;
                }
            }

            if (!ata_write_sectors(abs_lba(cluster_to_rel_sector(chain[i]) + s), 1, sector)) {
                terminal_puts("fat16: data write error\n");
                return 0;
            }

            offset += chunk;
            remaining -= chunk;
        }
    }

    return 1;
}

static int load_parent_dir_buf(const dir_ctx_t* parent, u8* buf, u32* out_size) {
    if (!parent || !buf) return 0;

    if (parent->is_root) {
        if (out_size) *out_size = ROOT_DIR_SECTORS * SECTOR_SIZE;
        return 1;
    }

    if (!dir_read_ctx(parent, buf, FAT16_MAX_LOAD_FILE_BYTES, out_size)) {
        return 0;
    }

    return 1;
}

static int dir_parent_cluster(const dir_ctx_t* dir, u16* out_parent_cluster) {
    if (!dir || !out_parent_cluster) return 0;
    u8 sector_buf[SECTOR_SIZE];

    if (dir->is_root) {
        *out_parent_cluster = 0;
        return 1;
    }

    u32 rel_sector = cluster_to_rel_sector(dir->start_cluster);
    if (!ata_read_sectors(abs_lba(rel_sector), 1, sector_buf)) {
        terminal_puts("fat16: directory read error\n");
        return 0;
    }

    *out_parent_cluster = read_u16_le(sector_buf + 32u, DIR_FIRST_CLUSTER);
    return 1;
}

static int dir_descends_from_cluster(dir_ctx_t dir, u16 ancestor_cluster) {
    if (ancestor_cluster < FIRST_CLUSTER) {
        return 0;
    }

    while (!dir.is_root && dir.start_cluster >= FIRST_CLUSTER) {
        if (dir.start_cluster == ancestor_cluster) {
            return 1;
        }

        u16 parent_cluster = 0;
        if (!dir_parent_cluster(&dir, &parent_cluster)) {
            return 0;
        }

        if (parent_cluster < FIRST_CLUSTER || parent_cluster == dir.start_cluster) {
            return 0;
        }

        dir.is_root = 0;
        dir.start_cluster = parent_cluster;
        dir.size = CLUSTER_BYTES;
    }

    return 0;
}

static int resolve_write_target(const char* path,
                                const char* fallback_leaf,
                                dir_ctx_t* out_parent,
                                char* out_leaf,
                                unsigned int out_leaf_size)
{
    resolved_path_t resolved;
    if (path && resolve_path(path, &resolved) && resolved.has_entry && entry_is_dir(resolved.entry)) {
        if (!fallback_leaf || !path_leaf_component(fallback_leaf, out_leaf, out_leaf_size)) {
            return 0;
        }
        if (out_parent) {
            *out_parent = dir_ctx_from_entry(resolved.entry);
        }
        return 1;
    }

    create_path_t create;
    if (!resolve_create_path(path, &create)) {
        return 0;
    }

    if (out_parent) {
        *out_parent = create.parent;
    }
    if (k_strlen(create.leaf) + 1u > out_leaf_size) {
        return 0;
    }
    k_memcpy(out_leaf, create.leaf, k_strlen(create.leaf) + 1u);
    return 1;
}

static int write_file_to_parent(const dir_ctx_t* parent,
                                const char* leaf,
                                const fat16_data_source_t* source,
                                u32 size) {
    if (!parent || !leaf || leaf[0] == '\0') {
        return 0;
    }
    if (size > 0 && (!source || !source->read)) {
        return 0;
    }

    if (size > FAT16_MAX_WRITE_FILE_BYTES) {
        terminal_puts("fat16: file too large\n");
        return 0;
    }

    u8 name83[11];
    to_83(leaf, name83);
    if (name83[0] == ' ') {
        return 0;
    }

    if (!load_fat_and_root()) {
        return 0;
    }

    u8* parent_buf = parent->is_root ? s_root_buf : s_dir_buf;
    u32 parent_size = parent->is_root ? (ROOT_DIR_SECTORS * SECTOR_SIZE)
                                      : parent->size;
    if (!load_parent_dir_buf(parent, parent_buf, &parent_size)) {
        return 0;
    }

    int existing_entry = -1;
    int free_entry = -1;
    u16 old_start_cluster = 0;
    u32 old_clusters = 0;

    for (u32 e = 0; e < parent_size / 32u; e++) {
        u8* entry = parent_buf + e * 32u;

        if (entry[DIR_NAME] == 0x00) {
            if (free_entry < 0) free_entry = (int)e;
            break;
        }

        if (entry[DIR_NAME] == 0xE5) {
            if (free_entry < 0) free_entry = (int)e;
            continue;
        }

        u8 attr = entry[DIR_ATTR];
        if (attr == ATTR_LONG_NAME || (attr & ATTR_VOLUME_ID)) {
            continue;
        }

        if (existing_entry < 0 && match_83(entry + DIR_NAME, leaf)) {
            if (entry_is_dir(entry)) {
                terminal_puts("fat16: not a file: ");
                terminal_puts(leaf);
                terminal_putc('\n');
                return 0;
            }
            existing_entry = (int)e;
            old_start_cluster = read_u16_le(entry, DIR_FIRST_CLUSTER);
        }
    }

    if (existing_entry < 0 && free_entry < 0) {
        terminal_puts("fat16: directory full\n");
        return 0;
    }

    u32 clusters_needed = 0;
    if (size > 0) {
        clusters_needed = (size + CLUSTER_BYTES - 1u) / CLUSTER_BYTES;
        if (clusters_needed > FAT16_MAX_WRITE_FILE_CLUSTERS) {
            terminal_puts("fat16: file too large\n");
            return 0;
        }
    }

    u16* fat = (u16*)s_fat_buf;
    if (existing_entry >= 0 && old_start_cluster >= FIRST_CLUSTER) {
        u32 cluster = (u32)old_start_cluster;
        while (cluster >= FIRST_CLUSTER &&
               cluster < FAT16_EOC_MIN &&
               old_clusters < FAT16_MAX_WRITE_FILE_CLUSTERS) {
            s_old_write_chain[old_clusters++] = cluster;
            u16 next = fat[cluster];
            fat[cluster] = 0;
            if (next >= FAT16_EOC_MIN) {
                break;
            }
            cluster = next;
        }
    }

    if (clusters_needed > 0 && !find_free_chain(fat, clusters_needed, s_write_chain)) {
        terminal_puts("fat16: filesystem full\n");

        if (old_clusters > 0) {
            for (u32 i = 0; i < old_clusters; i++) {
                u16 next = (i + 1 < old_clusters) ? (u16)s_old_write_chain[i + 1] : FAT16_EOC_MIN;
                fat[s_old_write_chain[i]] = next;
            }
        }

        return 0;
    }

    if (clusters_needed > 0 && !write_data_clusters(s_write_chain, clusters_needed, source, size)) {
        return 0;
    }

    if (clusters_needed > 0) {
        for (u32 i = 0; i < clusters_needed; i++) {
            u16 next = (i + 1 < clusters_needed) ? (u16)s_write_chain[i + 1] : FAT16_EOC_MIN;
            fat[s_write_chain[i]] = next;
        }
    }

    u8* entry = parent_buf + (u32)((existing_entry >= 0) ? existing_entry : free_entry) * 32u;
    write_dirent(entry, name83,
                 clusters_needed > 0 ? (u16)s_write_chain[0] : 0,
                 size);

    if (parent->is_root) {
        if (!write_fat_and_root()) {
            return 0;
        }
    } else {
        if (!dir_write_ctx(parent, parent_buf, parent_size)) {
            return 0;
        }
        if (!write_fat_and_root()) {
            return 0;
        }
    }

    return 1;
}

/*
 * match_83(dir_name, name)
 *
 * Compare an 11-byte space-padded 8.3 directory name against a
 * human-readable name like "hello" or "hello.elf".
 * Case-insensitive.  Returns 1 on match.
 */
static int match_83(const u8 dir_name[11], const char* name) {
    const char* dot = 0;
    for (const char* p = name; *p; p++) {
        if (*p == '.') dot = p;
    }

    u8 cmp[11];
    int i;
    for (i = 0; i < 11; i++) cmp[i] = ' ';

    const char* end_base;
    if (dot) {
        end_base = dot;
    } else {
        end_base = name + fat16_strlen(name);
    }

    int ni = 0;
    for (const char* p = name; p < end_base && ni < 8; p++, ni++)
        cmp[ni] = (u8)to_upper(*p);

    if (dot) {
        int ei = 0;
        for (const char* p = dot + 1; *p && ei < 3; p++, ei++)
            cmp[8 + ei] = (u8)to_upper(*p);
    }

    for (i = 0; i < 11; i++)
        if (dir_name[i] != cmp[i]) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* Public API                                                           */
/* ------------------------------------------------------------------ */

int fat16_init(void) {
    if (!s_load_buf) {
        s_load_buf = (u8*)kmalloc(FAT16_MAX_LOAD_FILE_BYTES);
        if (!s_load_buf) {
            terminal_puts("fat16: cannot allocate load buffer\n");
            return 0;
        }
    }

    /*
     * Step 1: read sector 0 of the whole disk to get the FAT16 LBA
     * from the MBR partition table.
     */
    if (!ata_read_sectors(0, 1, s_sector)) {
        terminal_puts("fat16: cannot read sector 0\n");
        return 0;
    }

    if (s_sector[510] != 0x55 || s_sector[511] != 0xAA) {
        terminal_puts("fat16: bad MBR signature\n");
        return 0;
    }

    u32 entry_off = MBR_PARTITION_TABLE_OFFSET +
                    FAT16_PARTITION_ENTRY_INDEX * MBR_PARTITION_ENTRY_SIZE;
    if (s_sector[entry_off + MBR_PARTITION_TYPE_OFFSET] != FAT16_PARTITION_TYPE) {
        terminal_puts("fat16: MBR partition type mismatch\n");
        return 0;
    }

    s_fat16_lba = read_u32_le(s_sector, entry_off + MBR_PARTITION_LBA_OFFSET);
    u32 fat16_sectors = read_u32_le(s_sector, entry_off + MBR_PARTITION_SIZE_OFFSET);

    if (s_fat16_lba == 0 || fat16_sectors == 0) {
        terminal_puts("fat16: partition entry not populated\n");
        return 0;
    }

    /*
     * Step 2: read the FAT16 boot sector and validate the BPB.
     */
    if (!ata_read_sectors(s_fat16_lba, 1, s_sector)) {
        terminal_puts("fat16: cannot read FAT16 boot sector\n");
        return 0;
    }

    if (s_sector[510] != 0x55 || s_sector[511] != 0xAA) {
        terminal_puts("fat16: bad boot signature\n");
        return 0;
    }

    u16 bytes_per_sector = read_u16_le(s_sector, 11);
    u8  secs_per_cluster = s_sector[13];
    u16 reserved         = read_u16_le(s_sector, 14);
    u8  num_fats         = s_sector[16];
    u16 root_entries     = read_u16_le(s_sector, 17);
    u16 fat_size         = read_u16_le(s_sector, 22);

    if (bytes_per_sector != SECTOR_SIZE ||
        secs_per_cluster != SECTORS_PER_CLUSTER ||
        reserved         != RESERVED_SECTORS ||
        num_fats         != NUM_FATS ||
        root_entries     != ROOT_ENTRY_COUNT ||
        fat_size         != FAT_SECTORS) {
        terminal_puts("fat16: BPB mismatch\n");
        return 0;
    }

    s_initialised = 1;

    terminal_puts("fat16: ok  lba=");
    terminal_put_uint(s_fat16_lba);
    terminal_putc('\n');
    return 1;
}

void fat16_ls_path(const char* path) {
    fat16_ls_path_filtered(path, 0);
}

void fat16_ls_path_filtered(const char* path, const char* pattern) {
    if (!s_initialised) { terminal_puts("fat16: not initialised\n"); return; }

    resolved_path_t resolved;
    if (!resolve_path(path, &resolved)) {
        terminal_puts("fat16: not found: ");
        terminal_puts(path ? path : "");
        terminal_putc('\n');
        return;
    }

    if (!resolved.has_entry) {
        if (!dir_list_ctx(&resolved.parent, "root directory", pattern)) {
            terminal_puts("fat16: directory read error\n");
        }
        return;
    }

    if (!entry_is_dir(resolved.entry)) {
        terminal_puts("fat16: not a directory: ");
        terminal_puts(path ? path : "");
        terminal_putc('\n');
        return;
    }

    dir_ctx_t dir = dir_ctx_from_entry(resolved.entry);
    if (!dir_list_ctx(&dir, path, pattern)) {
        terminal_puts("fat16: directory read error\n");
    }
}

void fat16_ls(void) {
    fat16_ls_path(0);
}

int fat16_stat(const char* name, u32* out_size) {
    if (!s_initialised) return 0;

    resolved_path_t resolved;
    if (!resolve_path(name, &resolved) || !resolved.has_entry) return 0;
    if (entry_is_dir(resolved.entry)) return 0;

    *out_size = read_u32_le(resolved.entry, DIR_FILE_SIZE);
    return 1;
}

const u8* fat16_load(const char* name, u32* out_size) {
    if (!s_initialised) { terminal_puts("fat16: not initialised\n"); return 0; }

    resolved_path_t resolved;
    if (!resolve_path(name, &resolved) || !resolved.has_entry) {
        terminal_puts("fat16: not found: ");
        terminal_puts(name);
        terminal_putc('\n');
        return 0;
    }

    if (entry_is_dir(resolved.entry)) {
        terminal_puts("fat16: not a file: ");
        terminal_puts(name);
        terminal_putc('\n');
        return 0;
    }

    u16 start_cluster = read_u16_le(resolved.entry, DIR_FIRST_CLUSTER);
    u32 file_size = read_u32_le(resolved.entry, DIR_FILE_SIZE);

    if (file_size > FAT16_MAX_LOAD_FILE_BYTES) {
        terminal_puts("fat16: file too large\n");
        return 0;
    }

    if (file_size == 0) {
        *out_size = 0;
        return s_load_buf;
    }

    u8 sector_buf[SECTOR_SIZE];
    u8* buf = s_load_buf;

    u32 bytes_remaining = file_size;
    u32 buf_offset      = 0;
    u32 cluster         = (u32)start_cluster;

    while (cluster >= FIRST_CLUSTER && cluster < FAT16_EOC_MIN) {
        u32 rel_sector = cluster_to_rel_sector(cluster);
        for (u32 s = 0; s < SECTORS_PER_CLUSTER; s++) {
            if (!ata_read_sectors(abs_lba(rel_sector + s), 1, sector_buf)) {
                terminal_puts("fat16: cluster read error\n");
                return 0;
            }

            u32 to_copy = bytes_remaining < SECTOR_SIZE
                        ? bytes_remaining : SECTOR_SIZE;
            k_memcpy(buf + buf_offset, sector_buf, to_copy);
            buf_offset      += to_copy;
            bytes_remaining -= to_copy;

            if (bytes_remaining == 0) {
                break;
            }
        }

        if (bytes_remaining == 0) break;

        cluster = (u32)fat_entry(cluster);
    }

    if (bytes_remaining != 0) {
        terminal_puts("fat16: chain ended early\n");
        return 0;
    }

    *out_size = file_size;
    return buf;
}

int fat16_read_path_to_sink(const char* name,
                            const fat16_data_sink_t* sink,
                            u32* out_size) {
    if (!s_initialised) { terminal_puts("fat16: not initialised\n"); return 0; }
    if (!sink || !sink->write || !out_size) return 0;

    resolved_path_t resolved;
    if (!resolve_path(name, &resolved) || !resolved.has_entry) {
        terminal_puts("fat16: not found: ");
        terminal_puts(name ? name : "");
        terminal_putc('\n');
        return 0;
    }

    if (entry_is_dir(resolved.entry)) {
        terminal_puts("fat16: not a file: ");
        terminal_puts(name ? name : "");
        terminal_putc('\n');
        return 0;
    }

    u16 start_cluster = read_u16_le(resolved.entry, DIR_FIRST_CLUSTER);
    u32 file_size = read_u32_le(resolved.entry, DIR_FILE_SIZE);
    *out_size = file_size;

    if (file_size == 0) {
        return 1;
    }

    u8 sector_buf[SECTOR_SIZE];
    u32 bytes_remaining = file_size;
    u32 file_offset = 0;
    u32 cluster = (u32)start_cluster;

    while (cluster >= FIRST_CLUSTER && cluster < FAT16_EOC_MIN) {
        u32 rel_sector = cluster_to_rel_sector(cluster);
        for (u32 s = 0; s < SECTORS_PER_CLUSTER; s++) {
            if (!ata_read_sectors(abs_lba(rel_sector + s), 1, sector_buf)) {
                terminal_puts("fat16: cluster read error\n");
                return 0;
            }

            u32 to_copy = bytes_remaining < SECTOR_SIZE
                        ? bytes_remaining : SECTOR_SIZE;
            if (to_copy > 0 && !sink->write(sink->ctx, file_offset, sector_buf, to_copy)) {
                terminal_puts("fat16: data sink write error\n");
                return 0;
            }

            file_offset += to_copy;
            bytes_remaining -= to_copy;
            if (bytes_remaining == 0) {
                break;
            }
        }

        if (bytes_remaining == 0) break;
        cluster = (u32)fat_entry(cluster);
    }

    if (bytes_remaining != 0) {
        terminal_puts("fat16: chain ended early\n");
        return 0;
    }

    return 1;
}

int fat16_write(const char* name, const u8* data, u32 size) {
    if (!s_initialised) {
        terminal_puts("fat16: not initialised\n");
        return 0;
    }

    if (!name || (!data && size > 0)) {
        return 0;
    }

    if (path_has_sep(name)) {
        terminal_puts("fat16: writes are root-directory only\n");
        return 0;
    }

    return fat16_write_path(name, data, size);
}

int fat16_write_path(const char* path, const u8* data, u32 size) {
    mem_source_t mem = { data };
    fat16_data_source_t source = { &mem, mem_source_read };
    return fat16_write_path_from_source(path, &source, size);
}

int fat16_write_path_from_source(const char* path,
                                 const fat16_data_source_t* source,
                                 u32 size) {
    if (!s_initialised) {
        terminal_puts("fat16: not initialised\n");
        return 0;
    }

    if (!path || (size > 0 && (!source || !source->read))) {
        return 0;
    }

    create_path_t create;
    if (!resolve_create_path(path, &create)) {
        terminal_puts("fat16: not found: ");
        terminal_puts(path ? path : "");
        terminal_putc('\n');
        return 0;
    }

    if (!path_leaf_to_83(create.leaf, (u8[11]){ 0 })) {
        terminal_puts("fat16: invalid file name\n");
        return 0;
    }

    return write_file_to_parent(&create.parent, create.leaf, source, size);
}

int fat16_copy(const char* src, const char* dst) {
    if (!s_initialised) {
        terminal_puts("fat16: not initialised\n");
        return 0;
    }

    if (!src || !dst) {
        return 0;
    }

    resolved_path_t resolved;
    if (!resolve_path(src, &resolved) || !resolved.has_entry) {
        terminal_puts("fat16: not found: ");
        terminal_puts(src);
        terminal_putc('\n');
        return 0;
    }

    if (entry_is_dir(resolved.entry)) {
        terminal_puts("fat16: not a file: ");
        terminal_puts(src);
        terminal_putc('\n');
        return 0;
    }

    u32 file_size = 0;
    const u8* data = fat16_load(src, &file_size);
    if (!data) {
        return 0;
    }

    char src_leaf[32];
    if (!path_leaf_component(src, src_leaf, sizeof(src_leaf))) {
        return 0;
    }

    dir_ctx_t target_parent;
    char target_leaf[32];
    if (!resolve_write_target(dst, src_leaf, &target_parent, target_leaf, sizeof(target_leaf))) {
        terminal_puts("fat16: not found: ");
        terminal_puts(dst);
        terminal_putc('\n');
        return 0;
    }

    mem_source_t mem = { data };
    fat16_data_source_t source = { &mem, mem_source_read };
    return write_file_to_parent(&target_parent, target_leaf, &source, file_size);
}

int fat16_move(const char* src, const char* dst) {
    if (!s_initialised) {
        terminal_puts("fat16: not initialised\n");
        return 0;
    }

    if (!src || !dst) {
        return 0;
    }

    resolved_path_t resolved;
    if (!resolve_path(src, &resolved) || !resolved.has_entry) {
        terminal_puts("fat16: not found: ");
        terminal_puts(src);
        terminal_putc('\n');
        return 0;
    }

    char src_leaf[32];
    if (!path_leaf_component(src, src_leaf, sizeof(src_leaf))) {
        return 0;
    }

    dir_ctx_t target_parent;
    char target_leaf[32];
    if (!resolve_write_target(dst, src_leaf, &target_parent, target_leaf, sizeof(target_leaf))) {
        terminal_puts("fat16: not found: ");
        terminal_puts(dst);
        terminal_putc('\n');
        return 0;
    }

    /*
     * File moves use the direct rename path below.  That keeps the operation
     * to a single metadata update instead of a copy + delete.
     */

    u8 target_name83[11];
    if (!path_leaf_to_83(target_leaf, target_name83)) {
        terminal_puts("fat16: invalid file name\n");
        return 0;
    }

    if (entry_is_dir(resolved.entry) &&
        !target_parent.is_root &&
        dir_descends_from_cluster(target_parent, read_u16_le(resolved.entry, DIR_FIRST_CLUSTER))) {
        terminal_puts("fat16: cannot move directory into itself\n");
        return 0;
    }

    if (!load_fat_and_root()) {
        return 0;
    }

    u8* src_parent_buf = resolved.parent.is_root ? s_root_buf : s_dir_buf;
    u32 src_parent_size = resolved.parent.is_root ? (ROOT_DIR_SECTORS * SECTOR_SIZE)
                                                  : resolved.parent.size;
    if (!load_parent_dir_buf(&resolved.parent, src_parent_buf, &src_parent_size)) {
        return 0;
    }

    u8* dst_parent_buf = target_parent.is_root ? s_root_buf : s_dir_buf2;
    u32 dst_parent_size = target_parent.is_root ? (ROOT_DIR_SECTORS * SECTOR_SIZE)
                                                : target_parent.size;
    if (!target_parent.is_root) {
        if (target_parent.is_root == resolved.parent.is_root &&
            target_parent.start_cluster == resolved.parent.start_cluster) {
            dst_parent_buf = src_parent_buf;
            dst_parent_size = src_parent_size;
        } else if (!load_parent_dir_buf(&target_parent, dst_parent_buf, &dst_parent_size)) {
            return 0;
        }
    }

    int src_slot = -1;
    int src_free = -1;
    char src_name[32];
    if (!path_leaf_component(src, src_name, sizeof(src_name))) {
        return 0;
    }
    if (!dir_find_slot_in_buf(src_parent_buf, src_parent_size, src_name, &src_slot, &src_free) || src_slot < 0) {
        terminal_puts("fat16: not found: ");
        terminal_puts(src);
        terminal_putc('\n');
        return 0;
    }

    int dst_slot = -1;
    int dst_free = -1;
    if (!dir_find_slot_in_buf(dst_parent_buf, dst_parent_size, target_leaf, &dst_slot, &dst_free)) {
        return 0;
    }

    if (dst_slot >= 0 && !(dst_parent_buf == src_parent_buf && dst_slot == src_slot)) {
        terminal_puts("fat16: already exists: ");
        terminal_puts(dst);
        terminal_putc('\n');
        return 0;
    }

    if (dst_slot < 0 && dst_free < 0) {
        terminal_puts("fat16: directory full\n");
        return 0;
    }

    if (dst_parent_buf == src_parent_buf && dst_slot == src_slot) {
        return 1;
    }

    u8 src_entry[32];
    k_memcpy(src_entry, resolved.entry, 32);
    write_dirent(dst_parent_buf + (u32)((dst_slot >= 0) ? dst_slot : dst_free) * 32u,
                 target_name83,
                 read_u16_le(src_entry, DIR_FIRST_CLUSTER),
                 read_u32_le(src_entry, DIR_FILE_SIZE));
    if (entry_is_dir(src_entry)) {
        dst_parent_buf[(u32)((dst_slot >= 0) ? dst_slot : dst_free) * 32u + DIR_ATTR] = ATTR_DIRECTORY;
    }

    src_parent_buf[(u32)src_slot * 32u] = 0xE5;

    if (entry_is_dir(src_entry) &&
        (resolved.parent.is_root != target_parent.is_root ||
         resolved.parent.start_cluster != target_parent.start_cluster)) {
        u16 parent_cluster = target_parent.is_root ? 0 : target_parent.start_cluster;
        u8* cluster_buf = s_load_buf;
        if (!read_cluster_data(read_u16_le(src_entry, DIR_FIRST_CLUSTER), cluster_buf)) {
            terminal_puts("fat16: directory read error\n");
            return 0;
        }
        write_u16_le(cluster_buf + 32u, DIR_FIRST_CLUSTER, parent_cluster);
        mem_source_t mem = { cluster_buf };
        fat16_data_source_t source = { &mem, mem_source_read };
        if (!write_data_clusters((const u32[]){ read_u16_le(src_entry, DIR_FIRST_CLUSTER) }, 1, &source, CLUSTER_BYTES)) {
            return 0;
        }
    }

    if (src_parent_buf == dst_parent_buf) {
        if (!resolved.parent.is_root) {
            if (!dir_write_ctx(&resolved.parent, src_parent_buf, src_parent_size)) {
                return 0;
            }
        }
    } else {
        if (!resolved.parent.is_root) {
            if (!dir_write_ctx(&resolved.parent, src_parent_buf, src_parent_size)) {
                return 0;
            }
        }
        if (!target_parent.is_root) {
            if (!dir_write_ctx(&target_parent, dst_parent_buf, dst_parent_size)) {
                return 0;
            }
        }
    }

    if (entry_is_dir(src_entry)) {
        if (!write_fat_and_root()) {
            return 0;
        }
        return 1;
    }

    if (resolved.parent.is_root || target_parent.is_root) {
        for (u32 s = 0; s < ROOT_DIR_SECTORS; s++) {
            if (!ata_write_sectors(abs_lba(ROOT_REL_SECTOR + s), 1, s_root_buf + s * SECTOR_SIZE)) {
                terminal_puts("fat16: root dir write error\n");
                return 0;
            }
        }
    }

    return 1;
}

int fat16_rm(const char* path) {
    if (!s_initialised) {
        terminal_puts("fat16: not initialised\n");
        return 0;
    }

    if (path_resolves_to_root(path)) {
        terminal_puts("fat16: cannot remove root\n");
        return 0;
    }

    resolved_path_t resolved;
    if (!resolve_path(path, &resolved) || !resolved.has_entry) {
        terminal_puts("fat16: not found: ");
        terminal_puts(path);
        terminal_putc('\n');
        return 0;
    }

    if (entry_is_dir(resolved.entry)) {
        terminal_puts("fat16: not a file: ");
        terminal_puts(path);
        terminal_putc('\n');
        return 0;
    }

    if (!load_fat_and_root()) {
        return 0;
    }

    u8* parent_buf = resolved.parent.is_root ? s_root_buf : s_dir_buf;
    u32 parent_size = resolved.parent.is_root ? (ROOT_DIR_SECTORS * SECTOR_SIZE)
                                              : resolved.parent.size;
    if (!resolved.parent.is_root) {
        if (!dir_read_ctx(&resolved.parent, parent_buf, CLUSTER_BYTES, 0)) {
            return 0;
        }
    }

    char leaf[32];
    if (!path_leaf_component(path, leaf, sizeof(leaf))) {
        terminal_puts("fat16: not found: ");
        terminal_puts(path);
        terminal_putc('\n');
        return 0;
    }

    int existing_slot = -1;
    int free_slot = -1;
    if (!dir_find_slot_in_buf(parent_buf, parent_size, leaf, &existing_slot, &free_slot)) {
        return 0;
    }

    if (existing_slot < 0) {
        terminal_puts("fat16: not found: ");
        terminal_puts(path);
        terminal_putc('\n');
        return 0;
    }

    parent_buf[(u32)existing_slot * 32u] = 0xE5;
    free_cluster_chain((u16*)s_fat_buf, read_u16_le(resolved.entry, DIR_FIRST_CLUSTER));

    if (!resolved.parent.is_root) {
        if (!dir_write_ctx(&resolved.parent, parent_buf, parent_size)) {
            return 0;
        }
    }

    if (!write_fat_and_root()) {
        return 0;
    }

    return 1;
}

int fat16_is_dir(const char* path) {
    if (!s_initialised) {
        return 0;
    }

    if (path_resolves_to_root(path)) {
        return 1;
    }

    resolved_path_t resolved;
    if (!resolve_path(path, &resolved) || !resolved.has_entry) {
        return 0;
    }

    return entry_is_dir(resolved.entry);
}

int fat16_mkdir(const char* path) {
    if (!s_initialised) {
        terminal_puts("fat16: not initialised\n");
        return 0;
    }

    create_path_t create;
    if (!resolve_create_path(path, &create)) {
        terminal_puts("fat16: not found: ");
        terminal_puts(path ? path : "");
        terminal_putc('\n');
        return 0;
    }

    u8 name83[11];
    if (!path_leaf_to_83(create.leaf, name83)) {
        terminal_puts("fat16: invalid directory name\n");
        return 0;
    }

    if (!load_fat_and_root()) {
        return 0;
    }

    u8* parent_buf = create.parent.is_root ? s_root_buf : s_dir_buf;
    u32 parent_size = create.parent.is_root ? (ROOT_DIR_SECTORS * SECTOR_SIZE)
                                            : create.parent.size;
    if (!create.parent.is_root) {
        if (!dir_read_ctx(&create.parent, parent_buf, CLUSTER_BYTES, 0)) {
            return 0;
        }
    }

    int existing_slot = -1;
    int free_slot = -1;
    if (!dir_find_slot_in_buf(parent_buf, parent_size, create.leaf, &existing_slot, &free_slot)) {
        return 0;
    }

    if (existing_slot >= 0) {
        terminal_puts("fat16: already exists: ");
        terminal_puts(path ? path : "");
        terminal_putc('\n');
        return 0;
    }

    if (free_slot < 0) {
        terminal_puts("fat16: directory full\n");
        return 0;
    }

    u32 chain[1];
    if (!find_free_chain((const u16*)s_fat_buf, 1, chain)) {
        terminal_puts("fat16: filesystem full\n");
        return 0;
    }

    u16 new_cluster = (u16)chain[0];
    u8* cluster_buf = s_load_buf;
    k_memset(cluster_buf, 0, CLUSTER_BYTES);
    write_dot_entry(cluster_buf, new_cluster, CLUSTER_BYTES, 0);
    write_dot_entry(cluster_buf + 32u,
                    create.parent.is_root ? 0 : create.parent.start_cluster,
                    create.parent.size,
                    1);

    mem_source_t mem = { cluster_buf };
    fat16_data_source_t source = { &mem, mem_source_read };
    if (!write_data_clusters(chain, 1, &source, CLUSTER_BYTES)) {
        return 0;
    }

    ((u16*)s_fat_buf)[new_cluster] = FAT16_EOC_MIN;

    write_dirent(parent_buf + (u32)free_slot * 32u, name83, new_cluster, CLUSTER_BYTES);
    parent_buf[(u32)free_slot * 32u + DIR_ATTR] = ATTR_DIRECTORY;

    if (!create.parent.is_root) {
        if (!dir_write_ctx(&create.parent, parent_buf, parent_size)) {
            return 0;
        }
    }

    if (!write_fat_and_root()) {
        return 0;
    }

    return 1;
}

int fat16_rmdir(const char* path) {
    if (!s_initialised) {
        terminal_puts("fat16: not initialised\n");
        return 0;
    }

    if (path_resolves_to_root(path)) {
        terminal_puts("fat16: cannot remove root\n");
        return 0;
    }

    resolved_path_t resolved;
    if (!resolve_path(path, &resolved) || !resolved.has_entry) {
        terminal_puts("fat16: not found: ");
        terminal_puts(path);
        terminal_putc('\n');
        return 0;
    }

    if (!entry_is_dir(resolved.entry)) {
        terminal_puts("fat16: not a directory: ");
        terminal_puts(path);
        terminal_putc('\n');
        return 0;
    }

    dir_ctx_t target = dir_ctx_from_entry(resolved.entry);
    if (target.start_cluster < FIRST_CLUSTER) {
        terminal_puts("fat16: cannot remove root\n");
        return 0;
    }

    u8* target_buf = s_dir_buf;
    u32 target_size = 0;
    if (!dir_read_ctx(&target, target_buf, CLUSTER_BYTES, &target_size)) {
        return 0;
    }

    if (dir_buf_has_entries(target_buf, target_size)) {
        terminal_puts("fat16: directory not empty\n");
        return 0;
    }

    if (!load_fat_and_root()) {
        return 0;
    }

    u8* parent_buf = resolved.parent.is_root ? s_root_buf : s_dir_buf2;
    u32 parent_size = resolved.parent.is_root ? (ROOT_DIR_SECTORS * SECTOR_SIZE)
                                              : resolved.parent.size;
    if (!resolved.parent.is_root) {
        if (!dir_read_ctx(&resolved.parent, parent_buf, CLUSTER_BYTES, 0)) {
            return 0;
        }
    }

    char leaf[32];
    if (!path_leaf_component(path, leaf, sizeof(leaf))) {
        terminal_puts("fat16: not found: ");
        terminal_puts(path);
        terminal_putc('\n');
        return 0;
    }

    int existing_slot = -1;
    int free_slot = -1;
    if (!dir_find_slot_in_buf(parent_buf, parent_size, leaf, &existing_slot, &free_slot)) {
        return 0;
    }

    if (existing_slot < 0) {
        terminal_puts("fat16: not found: ");
        terminal_puts(path);
        terminal_putc('\n');
        return 0;
    }

    parent_buf[(u32)existing_slot * 32u] = 0xE5;
    free_cluster_chain((u16*)s_fat_buf, target.start_cluster);

    if (!resolved.parent.is_root) {
        if (!dir_write_ctx(&resolved.parent, parent_buf, parent_size)) {
            return 0;
        }
    }

    if (!write_fat_and_root()) {
        return 0;
    }

    return 1;
}
