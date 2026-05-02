/*
 * mkfat16.c — SmallOS FAT16 image builder
 *
 * Usage:
 *   mkfat16 output.img [dest=]source ...
 *
 * Produces a raw FAT16 disk image (no partition table — the image IS
 * the FAT16 volume).  The image is a fixed 16 MB (32768 sectors).
 *
 * Layout:
 *
 *   Sector   0        Boot sector (BPB)
 *   Sectors  1–3      Reserved (4 reserved sectors total)
 *   Sectors  4–35     FAT 1  (32 sectors)
 *   Sectors 36–67     FAT 2  (mirror of FAT 1)
 *   Sectors 68–99     Root directory (32 sectors = 512 entries × 32 bytes)
 *   Sectors 100+      Data region  (cluster 2 = sectors 100–103, etc.)
 *
 * Entries are stored as a small directory tree.  If a destination path is
 * supplied, intermediate directories are created in the image; otherwise the
 * source basename is placed at the FAT16 root.  Filenames are stored in 8.3
 * format.
 *
 * Build:
 *   gcc -o build/tools/mkfat16 tools/mkfat16.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <ctype.h>

/* ------------------------------------------------------------------ */
/* Volume geometry (fixed)                                             */
/* ------------------------------------------------------------------ */

#define SECTOR_SIZE          512u
#define SECTORS_PER_CLUSTER  4u
#define RESERVED_SECTORS     4u
#define NUM_FATS             2u
#define FAT_SECTORS          32u         /* 32 sectors × 512B / 2B = 8192 FAT16 entries */
#define ROOT_ENTRY_COUNT     512u
#define ROOT_DIR_SECTORS     (ROOT_ENTRY_COUNT * 32u / SECTOR_SIZE)   /* = 32 */
#define TOTAL_SIZE_MB        16
#define TOTAL_SECTORS        32768       /* 16 MB */
#define VOLUME_LABEL         "SmallOS   "  /* 11 chars, space-padded */
#define MAX_FILES            64

/* Derived sector offsets */
#define FAT1_START    RESERVED_SECTORS
#define FAT2_START    (FAT1_START + FAT_SECTORS)
#define ROOT_START    (FAT2_START + FAT_SECTORS)
#define DATA_START    (ROOT_START + ROOT_DIR_SECTORS)  /* = 4+32+32+32 = 100 */

/* FAT16 cluster conventions */
#define FIRST_CLUSTER  2u
#define FAT16_EOC      0xFFFFu

#define FAT_ENTRIES    (FAT_SECTORS * SECTOR_SIZE / 2u)   /* 8192 */

#define CLUSTER_BYTES  (SECTORS_PER_CLUSTER * SECTOR_SIZE)   /* 2048 */

/* ------------------------------------------------------------------ */
/* Types                                                               */
/* ------------------------------------------------------------------ */

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

/* ------------------------------------------------------------------ */
/* Little-endian write helpers                                         */
/* ------------------------------------------------------------------ */

static void put_u8(u8* buf, u32 off, u8 val) {
    buf[off] = val;
}

static void put_u16(u8* buf, u32 off, u16 val) {
    buf[off + 0] = (u8)(val);
    buf[off + 1] = (u8)(val >> 8);
}

static void put_u32(u8* buf, u32 off, u32 val) {
    buf[off + 0] = (u8)(val);
    buf[off + 1] = (u8)(val >> 8);
    buf[off + 2] = (u8)(val >> 16);
    buf[off + 3] = (u8)(val >> 24);
}

/* ------------------------------------------------------------------ */
/* Boot sector / BPB                                                   */
/* ------------------------------------------------------------------ */

static void build_boot_sector(u8* sector) {
    memset(sector, 0, SECTOR_SIZE);

    /* Jump + NOP */
    sector[0] = 0xEB;
    sector[1] = 0x58;
    sector[2] = 0x90;

    /* OEM identifier */
    memcpy(sector + 3, "SmallOS", 8);

    /* BPB */
    put_u16(sector, 11, SECTOR_SIZE);
    put_u8 (sector, 13, SECTORS_PER_CLUSTER);
    put_u16(sector, 14, RESERVED_SECTORS);
    put_u8 (sector, 16, NUM_FATS);
    put_u16(sector, 17, ROOT_ENTRY_COUNT);
    put_u16(sector, 19, (u16)TOTAL_SECTORS);
    put_u8 (sector, 21, 0xF8);            /* media: fixed disk */
    put_u16(sector, 22, FAT_SECTORS);
    put_u16(sector, 24, 63);              /* sectors/track */
    put_u16(sector, 26, 255);             /* heads */
    put_u32(sector, 28, 0);              /* hidden sectors */
    put_u32(sector, 32, 0);              /* total sectors 32-bit (0 = use 16-bit) */

    /* Extended BPB */
    put_u8 (sector, 36, 0x80);           /* drive number */
    put_u8 (sector, 37, 0x00);
    put_u8 (sector, 38, 0x29);           /* extended boot sig */
    put_u32(sector, 39, 0x12345678);     /* volume serial */
    memcpy (sector + 43, VOLUME_LABEL, 11);
    memcpy (sector + 54, "FAT16   ", 8);

    /* Boot signature */
    sector[510] = 0x55;
    sector[511] = 0xAA;
}

/* ------------------------------------------------------------------ */
/* FAT                                                                 */
/* ------------------------------------------------------------------ */

static u16 s_fat[FAT_ENTRIES];

static void fat_init(void) {
    memset(s_fat, 0, sizeof(s_fat));
    s_fat[0] = 0xFFF8;   /* media descriptor */
    s_fat[1] = 0xFFFF;   /* reserved */
}

static void fat_alloc_chain(u32 start, u32 count) {
    for (u32 i = 0; i < count; i++) {
        u32 c = start + i;
        s_fat[c] = (i + 1 < count) ? (u16)(c + 1) : FAT16_EOC;
    }
}

static void fat_serialise(u8* out_buf) {
    /* Write FAT as little-endian u16 array into out_buf */
    for (u32 i = 0; i < FAT_ENTRIES; i++) {
        out_buf[i * 2 + 0] = (u8)(s_fat[i]);
        out_buf[i * 2 + 1] = (u8)(s_fat[i] >> 8);
    }
}

/* ------------------------------------------------------------------ */
/* 8.3 filename conversion                                             */
/* ------------------------------------------------------------------ */

static void to_83(const char* path, u8 name83[11]) {
    /* Find basename */
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (*p == '/' || *p == '\\') base = p + 1;
    }

    /* Find last dot */
    const char* dot = NULL;
    for (const char* p = base; *p; p++) {
        if (*p == '.') dot = p;
    }

    memset(name83, ' ', 11);

    /* Name (up to 8 chars) */
    const char* name_end = dot ? dot : base + strlen(base);
    int ni = 0;
    for (const char* p = base; p < name_end && ni < 8; p++, ni++) {
        name83[ni] = (u8)toupper((unsigned char)*p);
    }

    /* Extension (up to 3 chars) */
    if (dot) {
        int ei = 0;
        for (const char* p = dot + 1; *p && ei < 3; p++, ei++) {
            name83[8 + ei] = (u8)toupper((unsigned char)*p);
        }
    }
}

#define PATH_MAX_CHARS 256

typedef struct node node_t;

typedef struct child_ref {
    node_t* node;
    struct child_ref* next;
} child_ref_t;

struct node {
    u8 name83[11];
    int is_dir;
    const char* src_path;
    u32 size;
    u32 cluster;
    u32 cluster_count;
    node_t* parent;
    child_ref_t* children;
    child_ref_t* child_tail;
};

static void die(const char* msg);
static u32 measure_file(const char* path);

static int is_sep(char c) {
    return c == '/' || c == '\\';
}

static const char* basename_ptr(const char* path) {
    const char* base = path;
    for (const char* p = path; *p; p++) {
        if (is_sep(*p)) base = p + 1;
    }
    return base;
}

static void copy_path_base(char* dst, size_t dst_size, const char* src) {
    const char* base = basename_ptr(src);
    size_t i = 0;
    while (base[i] && i + 1 < dst_size) {
        dst[i] = base[i];
        i++;
    }
    dst[i] = '\0';
}

static const char* parse_spec(const char* spec, char* dest, size_t dest_size) {
    const char* eq = strchr(spec, '=');
    if (!eq) {
        copy_path_base(dest, dest_size, spec);
        return spec;
    }

    size_t dest_len = (size_t)(eq - spec);
    if (dest_len >= dest_size) {
        return 0;
    }
    memcpy(dest, spec, dest_len);
    dest[dest_len] = '\0';
    return eq + 1;
}

static node_t* node_create(int is_dir, const char* src_path) {
    node_t* node = calloc(1, sizeof(node_t));
    if (!node) die("out of memory");
    node->is_dir = is_dir;
    node->src_path = src_path;
    return node;
}

static node_t* node_find_child(node_t* parent, const u8 name83[11]) {
    for (child_ref_t* ref = parent->children; ref; ref = ref->next) {
        if (memcmp(ref->node->name83, name83, 11) == 0) {
            return ref->node;
        }
    }
    return 0;
}

static node_t* node_add_child(node_t* parent, node_t* child) {
    child_ref_t* ref = calloc(1, sizeof(child_ref_t));
    if (!ref) die("out of memory");
    ref->node = child;
    if (!parent->children) {
        parent->children = ref;
    } else {
        parent->child_tail->next = ref;
    }
    parent->child_tail = ref;
    child->parent = parent;
    return child;
}

static void ensure_directory_path(node_t* root, const char* path, node_t** out_parent) {
    node_t* cur = root;
    const char* p = path;
    char component[PATH_MAX_CHARS];

    while (*p) {
        while (*p && is_sep(*p)) p++;
        if (!*p) break;

        size_t len = 0;
        while (p[len] && !is_sep(p[len])) len++;
        if (len == 0 || len >= sizeof(component)) {
            die("path component too long");
        }

        memcpy(component, p, len);
        component[len] = '\0';
        p += len;

        while (*p && is_sep(*p)) p++;

        if (*p == '\0') {
            *out_parent = cur;
            return;
        }

        u8 name83[11];
        to_83(component, name83);
        if (name83[0] == ' ') die("invalid directory name");

        node_t* next = node_find_child(cur, name83);
        if (!next) {
            next = node_create(1, 0);
            memcpy(next->name83, name83, 11);
            node_add_child(cur, next);
        } else if (!next->is_dir) {
            die("path component collides with file");
        }
        cur = next;
    }

    *out_parent = cur;
}

static void add_file_entry(node_t* root, const char* dest_path, const char* src_path) {
    node_t* parent = 0;
    ensure_directory_path(root, dest_path, &parent);

    const char* base = basename_ptr(dest_path);
    u8 name83[11];
    to_83(base, name83);
    if (name83[0] == ' ') die("invalid file name");

    if (node_find_child(parent, name83)) {
        die("duplicate destination path");
    }

    node_t* file = node_create(0, src_path);
    memcpy(file->name83, name83, 11);
    file->size = measure_file(src_path);
    node_add_child(parent, file);
}

static void compute_layout(node_t* node) {
    if (!node->is_dir) {
        if (node->size == 0) {
            node->cluster_count = 1;
        } else {
            node->cluster_count = (node->size + CLUSTER_BYTES - 1u) / CLUSTER_BYTES;
            if (node->cluster_count == 0) node->cluster_count = 1;
        }
        return;
    }

    u32 child_count = 0;
    for (child_ref_t* ref = node->children; ref; ref = ref->next) {
        compute_layout(ref->node);
        child_count++;
    }

    u32 used_size = (2u + child_count) * 32u;
    /*
     * Give directories a full cluster-sized backing store instead of
     * compacting them to the exact used entry count.  That keeps
     * shipped directories writable at runtime without immediately
     * reporting "directory full" when a new file is copied into them.
     */
    node->cluster_count = (used_size + CLUSTER_BYTES - 1u) / CLUSTER_BYTES;
    if (node->cluster_count == 0) node->cluster_count = 1;
    node->size = node->cluster_count * CLUSTER_BYTES;
}

static u32 assign_clusters(node_t* node, u32 next_cluster) {
    if (!node->is_dir) {
        node->cluster = next_cluster;
        return next_cluster + node->cluster_count;
    }

    if (node->parent != 0) {
        node->cluster = next_cluster;
        next_cluster += node->cluster_count;
    }

    for (child_ref_t* ref = node->children; ref; ref = ref->next) {
        next_cluster = assign_clusters(ref->node, next_cluster);
    }

    return next_cluster;
}

static void mark_fat_chain(u16* fat, u32 start, u32 count) {
    for (u32 i = 0; i < count; i++) {
        u32 cluster = start + i;
        fat[cluster] = (i + 1 < count) ? (u16)(cluster + 1) : FAT16_EOC;
    }
}

static u32 count_nodes(node_t* node) {
    if (!node->is_dir) return 1;
    u32 total = 0;
    for (child_ref_t* ref = node->children; ref; ref = ref->next) {
        total += count_nodes(ref->node);
    }
    return total;
}

/* ------------------------------------------------------------------ */
/* Directory entry                                                     */
/* ------------------------------------------------------------------ */

static void write_dirent(u8* entry, const u8 name83[11],
                         u8 attr, u16 start_cluster, u32 file_size) {
    memset(entry, 0, 32);
    memcpy(entry,      name83, 11);
    entry[11] = attr;
    put_u16(entry, 26, start_cluster);
    put_u32(entry, 28, file_size);
}

/* ------------------------------------------------------------------ */
/* Error helper                                                        */
/* ------------------------------------------------------------------ */

static void die(const char* msg) {
    fprintf(stderr, "mkfat16: %s\n", msg);
    exit(1);
}

static void write_dot_entry(u8* entry, u16 cluster, u32 size, int is_dotdot) {
    memset(entry, 0, 32);
    entry[0] = '.';
    if (is_dotdot) {
        entry[1] = '.';
    }
    entry[11] = 0x10;
    put_u16(entry, 26, cluster);
    put_u32(entry, 28, size);
}

static void emit_directory_contents(node_t* dir, u8* buf, u32 buf_size) {
    memset(buf, 0, buf_size);
    u32 entry_index = 0;

    if (dir->parent != 0) {
        if ((entry_index + 1u) * 32u > buf_size) die("directory too large");
        write_dot_entry(buf + entry_index * 32u, (u16)dir->cluster, dir->size, 0);
        entry_index++;

        u16 parent_cluster = 0;
        if (dir->parent->parent != 0) {
            parent_cluster = (u16)dir->parent->cluster;
        }
        if ((entry_index + 1u) * 32u > buf_size) die("directory too large");
        write_dot_entry(buf + entry_index * 32u, parent_cluster,
                        dir->parent->is_dir ? dir->parent->size : dir->parent->size,
                        1);
        entry_index++;
    }

    for (child_ref_t* ref = dir->children; ref; ref = ref->next) {
        node_t* child = ref->node;
        u8 attr = child->is_dir ? 0x10 : 0x20;
        if ((entry_index + 1u) * 32u > buf_size) die("directory too large");
        write_dirent(buf + entry_index * 32u,
                     child->name83,
                     attr,
                     (u16)child->cluster,
                     child->is_dir ? child->size : child->size);
        entry_index++;
    }
}

static void emit_file_data(node_t* file, FILE* out) {
    FILE* f = fopen(file->src_path, "rb");
    if (!f) {
        fprintf(stderr, "mkfat16: cannot open '%s'\n", file->src_path);
        exit(1);
    }

    u8* cbuf = calloc(CLUSTER_BYTES, 1);
    if (!cbuf) die("out of memory");

    u32 remaining = file->size;
    for (u32 c = 0; c < file->cluster_count; c++) {
        memset(cbuf, 0, CLUSTER_BYTES);
        u32 to_read = remaining < CLUSTER_BYTES ? remaining : CLUSTER_BYTES;
        if (to_read > 0) {
            size_t got = fread(cbuf, 1, to_read, f);
            if (got != to_read) {
                fprintf(stderr, "mkfat16: read error on '%s'\n", file->src_path);
                fclose(f);
                free(cbuf);
                exit(1);
            }
            remaining -= (u32)got;
        }
        if (fwrite(cbuf, 1, CLUSTER_BYTES, out) != CLUSTER_BYTES) die("write failed");
    }

    free(cbuf);
    fclose(f);
}

static void emit_directory_data(node_t* dir, FILE* out) {
    u32 bytes = dir->cluster_count * CLUSTER_BYTES;
    u8* buf = calloc(bytes, 1);
    if (!buf) die("out of memory");

    emit_directory_contents(dir, buf, bytes);
    if (fwrite(buf, 1, bytes, out) != bytes) die("write failed");
    free(buf);
}

static void emit_tree_data(node_t* node, FILE* out) {
    if (!node->is_dir) {
        emit_file_data(node, out);
        return;
    }

    if (node->parent != 0) {
        emit_directory_data(node, out);
    }

    for (child_ref_t* ref = node->children; ref; ref = ref->next) {
        emit_tree_data(ref->node, out);
    }
}

static void mark_tree_fat(node_t* node, u16* fat) {
    if (node->parent != 0) {
        mark_fat_chain(fat, node->cluster, node->cluster_count);
    }

    if (!node->is_dir) {
        return;
    }

    for (child_ref_t* ref = node->children; ref; ref = ref->next) {
        mark_tree_fat(ref->node, fat);
    }
}

static u32 data_sector_count(node_t* node) {
    u32 total = 0;

    if (node->parent != 0) {
        total += node->cluster_count * SECTORS_PER_CLUSTER;
    }

    if (!node->is_dir) {
        return total;
    }

    for (child_ref_t* ref = node->children; ref; ref = ref->next) {
        total += data_sector_count(ref->node);
    }

    return total;
}

static u32 measure_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "mkfat16: cannot open '%s'\n", path);
        exit(1);
    }
    if (fseek(f, 0, SEEK_END) != 0) die("fseek failed");
    long sz = ftell(f);
    if (sz < 0) die("ftell failed");
    fclose(f);
    return (u32)sz;
}

/* ------------------------------------------------------------------ */
/* main                                                                */
/* ------------------------------------------------------------------ */

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: mkfat16 output.img [dest=]source ...\n");
        return 1;
    }

    const char* output_path = argv[1];
    int nspecs = argc - 2;
    if (nspecs > MAX_FILES) die("too many files (max 64)");

    node_t root_node;
    memset(&root_node, 0, sizeof(root_node));
    root_node.is_dir = 1;

    for (int i = 0; i < nspecs; i++) {
        char dest[PATH_MAX_CHARS];
        const char* src = parse_spec(argv[2 + i], dest, sizeof(dest));
        if (!src) die("invalid destination path");
        add_file_entry(&root_node, dest, src);
    }

    compute_layout(&root_node);
    u32 next_cluster = assign_clusters(&root_node, FIRST_CLUSTER);
    u32 max_data_clusters = (TOTAL_SECTORS - DATA_START) / SECTORS_PER_CLUSTER;
    if (next_cluster > FIRST_CLUSTER + max_data_clusters) {
        die("filesystem full");
    }

    fat_init();
    mark_tree_fat(&root_node, s_fat);

    FILE* out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "mkfat16: cannot open output '%s'\n", output_path);
        return 1;
    }

    /* --- Sector 0: boot sector --- */
    u8 boot[SECTOR_SIZE];
    build_boot_sector(boot);
    if (fwrite(boot, 1, SECTOR_SIZE, out) != SECTOR_SIZE) die("write failed");

    /* --- Sectors 1–3: reserved (zero) --- */
    u8 zero_sector[SECTOR_SIZE];
    memset(zero_sector, 0, sizeof(zero_sector));
    for (u32 s = 1; s < RESERVED_SECTORS; s++) {
        if (fwrite(zero_sector, 1, SECTOR_SIZE, out) != SECTOR_SIZE) die("write failed");
    }

    /* --- FAT1 and FAT2 --- */
    u8* fat_buf = calloc(FAT_SECTORS * SECTOR_SIZE, 1);
    if (!fat_buf) die("out of memory");
    fat_serialise(fat_buf);
    /* FAT1 */
    if (fwrite(fat_buf, 1, FAT_SECTORS * SECTOR_SIZE, out)
            != FAT_SECTORS * SECTOR_SIZE) die("write failed");
    /* FAT2 (identical copy) */
    if (fwrite(fat_buf, 1, FAT_SECTORS * SECTOR_SIZE, out)
            != FAT_SECTORS * SECTOR_SIZE) die("write failed");
    free(fat_buf);

    /* --- Root directory --- */
    u8* root_buf = calloc(ROOT_DIR_SECTORS * SECTOR_SIZE, 1);
    if (!root_buf) die("out of memory");

    emit_directory_contents(&root_node, root_buf, ROOT_DIR_SECTORS * SECTOR_SIZE);
    if (fwrite(root_buf, 1, ROOT_DIR_SECTORS * SECTOR_SIZE, out)
            != ROOT_DIR_SECTORS * SECTOR_SIZE) die("write failed");
    free(root_buf);

    emit_tree_data(&root_node, out);

    u32 written_sectors = DATA_START + data_sector_count(&root_node);
    if (written_sectors < TOTAL_SECTORS) {
        u8* pad = calloc(CLUSTER_BYTES, 1);
        if (!pad) die("out of memory");
        while (written_sectors < TOTAL_SECTORS) {
            u32 left = TOTAL_SECTORS - written_sectors;
            u32 write_sectors = left < SECTORS_PER_CLUSTER ? left : SECTORS_PER_CLUSTER;
            u32 write_bytes = write_sectors * SECTOR_SIZE;
            if (fwrite(pad, 1, write_bytes, out) != write_bytes) die("write failed");
            written_sectors += write_sectors;
        }
        free(pad);
    }

    fclose(out);

    fprintf(stdout, "fat16: %s  %u sector volume\n",
            output_path, TOTAL_SECTORS);
    return 0;
}
