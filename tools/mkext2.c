/*
 * mkext2.c - SmallOS ext2 image builder
 *
 * Usage:
 *   mkext2 output.img [dest=]source ... dir/ ...
 *
 * Produces a deterministic 16 MB raw ext2 volume with 4 KiB blocks,
 * 128-byte inodes, one block group, and native directory names.
 */

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TOTAL_SIZE_MB        16
#define TOTAL_BYTES          (TOTAL_SIZE_MB * 1024u * 1024u)
#define BLOCK_SIZE           4096u
#define TOTAL_BLOCKS         (TOTAL_BYTES / BLOCK_SIZE)
#define TOTAL_INODES         512u
#define INODE_SIZE           128u
#define ROOT_INO             2u
#define FIRST_NONRES_INO     11u
#define SUPER_MAGIC          0xEF53u

#define GROUP_DESC_BLOCK     1u
#define BLOCK_BITMAP_BLOCK   2u
#define INODE_BITMAP_BLOCK   3u
#define INODE_TABLE_BLOCK    4u
#define INODE_TABLE_BLOCKS   16u
#define FIRST_DATA_BLOCK     20u

#define EXT2_S_IFREG         0x8000u
#define EXT2_S_IFDIR         0x4000u
#define EXT2_FT_REG_FILE     1u
#define EXT2_FT_DIR          2u
#define PTRS_PER_BLOCK       (BLOCK_SIZE / 4u)
#define MAX_SPECS            384
#define PATH_MAX_CHARS       256

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;

typedef struct node node_t;

typedef struct child_ref {
    node_t* node;
    struct child_ref* next;
} child_ref_t;

struct node {
    char name[256];
    int is_dir;
    const char* src_path;
    u32 size;
    u32 ino;
    u32 blocks_needed;
    u32 data_blocks[4096];
    node_t* parent;
    child_ref_t* children;
    child_ref_t* child_tail;
};

static u8* s_img;
static u32 s_next_ino = FIRST_NONRES_INO;
static u32 s_next_block = FIRST_DATA_BLOCK;

static void die(const char* msg) {
    fprintf(stderr, "mkext2: %s\n", msg);
    exit(1);
}

static void put_u16(u8* buf, u32 off, u16 val) {
    buf[off] = (u8)(val & 0xFFu);
    buf[off + 1u] = (u8)((val >> 8) & 0xFFu);
}

static void put_u32(u8* buf, u32 off, u32 val) {
    buf[off] = (u8)(val & 0xFFu);
    buf[off + 1u] = (u8)((val >> 8) & 0xFFu);
    buf[off + 2u] = (u8)((val >> 16) & 0xFFu);
    buf[off + 3u] = (u8)((val >> 24) & 0xFFu);
}

static u32 get_u32(const u8* buf, u32 off) {
    return (u32)buf[off] | ((u32)buf[off + 1u] << 8) |
           ((u32)buf[off + 2u] << 16) | ((u32)buf[off + 3u] << 24);
}

static u8* block_ptr(u32 block) {
    return s_img + block * BLOCK_SIZE;
}

static u8* inode_ptr(u32 ino) {
    u32 index = ino - 1u;
    u32 byte_off = index * INODE_SIZE;
    return block_ptr(INODE_TABLE_BLOCK + byte_off / BLOCK_SIZE) + (byte_off % BLOCK_SIZE);
}

static void bit_set(u8* bits, u32 index) {
    bits[index / 8u] = (u8)(bits[index / 8u] | (u8)(1u << (index % 8u)));
}

static int is_sep(char c) {
    return c == '/' || c == '\\';
}

static int path_ends_with_sep(const char* path) {
    size_t len = path ? strlen(path) : 0;
    return len > 0 && is_sep(path[len - 1u]);
}

static void trim_trailing_seps(char* path) {
    size_t len = strlen(path);
    while (len > 0 && is_sep(path[len - 1u])) path[--len] = '\0';
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
    while (base[i] && i + 1u < dst_size) {
        dst[i] = base[i];
        i++;
    }
    dst[i] = '\0';
}

static const char* parse_spec(const char* spec, char* dest, size_t dest_size,
                              int* out_is_dir) {
    const char* eq = strchr(spec, '=');
    if (out_is_dir) *out_is_dir = 0;

    if (!eq) {
        if (path_ends_with_sep(spec)) {
            size_t len = strlen(spec);
            if (len >= dest_size) return 0;
            memcpy(dest, spec, len + 1u);
            trim_trailing_seps(dest);
            if (out_is_dir) *out_is_dir = 1;
            return 0;
        }
        copy_path_base(dest, dest_size, spec);
        return spec;
    }

    size_t dest_len = (size_t)(eq - spec);
    if (dest_len >= dest_size) return 0;
    memcpy(dest, spec, dest_len);
    dest[dest_len] = '\0';
    if (eq[1] == '\0' && path_ends_with_sep(dest)) {
        trim_trailing_seps(dest);
        if (out_is_dir) *out_is_dir = 1;
        return 0;
    }
    return eq + 1;
}

static u32 measure_file(const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) {
        fprintf(stderr, "mkext2: cannot open '%s'\n", path);
        exit(1);
    }
    if (fseek(f, 0, SEEK_END) != 0) die("fseek failed");
    long sz = ftell(f);
    if (sz < 0) die("ftell failed");
    fclose(f);
    return (u32)sz;
}

static node_t* node_create(const char* name, int is_dir, const char* src_path) {
    node_t* node = (node_t*)calloc(1, sizeof(node_t));
    if (!node) die("out of memory");
    if (name) {
        if (strlen(name) > 255u) die("name too long");
        strcpy(node->name, name);
    }
    node->is_dir = is_dir;
    node->src_path = src_path;
    if (!is_dir && src_path) node->size = measure_file(src_path);
    return node;
}

static node_t* node_find_child(node_t* parent, const char* name) {
    for (child_ref_t* ref = parent->children; ref; ref = ref->next) {
        if (strcmp(ref->node->name, name) == 0) return ref->node;
    }
    return 0;
}

static node_t* node_add_child(node_t* parent, node_t* child) {
    child_ref_t* ref = (child_ref_t*)calloc(1, sizeof(child_ref_t));
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

static void split_next(const char** path, char* out, size_t out_size, int* is_last) {
    const char* p = *path;
    while (*p && is_sep(*p)) p++;
    size_t len = 0;
    while (p[len] && !is_sep(p[len])) len++;
    if (len == 0 || len >= out_size) die("invalid path component");
    memcpy(out, p, len);
    out[len] = '\0';
    p += len;
    while (*p && is_sep(*p)) p++;
    *is_last = (*p == '\0');
    *path = p;
}

static void add_directory_entry(node_t* root, const char* dest_path) {
    node_t* cur = root;
    const char* p = dest_path;
    while (*p) {
        char component[256];
        int is_last = 0;
        split_next(&p, component, sizeof(component), &is_last);
        node_t* next = node_find_child(cur, component);
        if (!next) {
            next = node_create(component, 1, 0);
            node_add_child(cur, next);
        } else if (!next->is_dir) {
            die("directory path collides with file");
        }
        cur = next;
    }
    if (cur == root) die("invalid directory name");
}

static void ensure_directory_path(node_t* root, const char* path, node_t** out_parent) {
    node_t* cur = root;
    const char* p = path;
    while (*p) {
        char component[256];
        int is_last = 0;
        split_next(&p, component, sizeof(component), &is_last);
        if (is_last) {
            *out_parent = cur;
            return;
        }
        node_t* next = node_find_child(cur, component);
        if (!next) {
            next = node_create(component, 1, 0);
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
    if (!base[0]) die("invalid file name");
    if (node_find_child(parent, base)) die("duplicate destination path");
    node_add_child(parent, node_create(base, 0, src_path));
}

static u32 dir_entry_need(const char* name) {
    return (8u + (u32)strlen(name) + 3u) & ~3u;
}

static u32 dir_used_bytes(node_t* dir) {
    u32 used = dir_entry_need(".") + dir_entry_need("..");
    for (child_ref_t* ref = dir->children; ref; ref = ref->next) {
        used += dir_entry_need(ref->node->name);
    }
    return used;
}

static void compute_layout(node_t* node) {
    if (node->is_dir) {
        for (child_ref_t* ref = node->children; ref; ref = ref->next) {
            compute_layout(ref->node);
        }
        u32 used = dir_used_bytes(node);
        node->size = BLOCK_SIZE;
        node->blocks_needed = (used + BLOCK_SIZE - 1u) / BLOCK_SIZE;
        if (node->blocks_needed == 0) node->blocks_needed = 1;
        return;
    }

    node->blocks_needed = (node->size + BLOCK_SIZE - 1u) / BLOCK_SIZE;
    if (node->blocks_needed == 0) node->blocks_needed = 1;
}

static void assign_inodes(node_t* node) {
    if (node->parent == 0) {
        node->ino = ROOT_INO;
    } else {
        if (s_next_ino > TOTAL_INODES) die("out of inodes");
        node->ino = s_next_ino++;
    }
    for (child_ref_t* ref = node->children; ref; ref = ref->next) {
        assign_inodes(ref->node);
    }
}

static u32 pointer_blocks_for(u32 data_blocks) {
    if (data_blocks <= 12u) return 0;
    if (data_blocks <= 12u + PTRS_PER_BLOCK) return 1;
    u32 rem = data_blocks - 12u - PTRS_PER_BLOCK;
    return 1u + 1u + (rem + PTRS_PER_BLOCK - 1u) / PTRS_PER_BLOCK;
}

static void assign_blocks(node_t* node) {
    u32 extra = pointer_blocks_for(node->blocks_needed);
    if (s_next_block + node->blocks_needed + extra > TOTAL_BLOCKS) die("filesystem full");
    for (u32 i = 0; i < node->blocks_needed; i++) {
        node->data_blocks[i] = s_next_block++;
    }
    s_next_block += extra;

    for (child_ref_t* ref = node->children; ref; ref = ref->next) {
        assign_blocks(ref->node);
    }
}

static void write_dirent(u8* buf, u32 off, u32 ino, u16 rec_len,
                         const char* name, u8 file_type) {
    size_t name_len = strlen(name);
    put_u32(buf, off, ino);
    put_u16(buf, off + 4u, rec_len);
    buf[off + 6u] = (u8)name_len;
    buf[off + 7u] = file_type;
    memcpy(buf + off + 8u, name, name_len);
}

static void write_directory(node_t* dir) {
    u8* buf = block_ptr(dir->data_blocks[0]);
    memset(buf, 0, BLOCK_SIZE);
    u32 off = 0;

    write_dirent(buf, off, dir->ino, (u16)dir_entry_need("."), ".", EXT2_FT_DIR);
    off += dir_entry_need(".");
    u32 parent_ino = dir->parent ? dir->parent->ino : dir->ino;
    write_dirent(buf, off, parent_ino, (u16)dir_entry_need(".."), "..", EXT2_FT_DIR);
    off += dir_entry_need("..");

    for (child_ref_t* ref = dir->children; ref; ref = ref->next) {
        node_t* child = ref->node;
        u16 rec_len = (u16)dir_entry_need(child->name);
        if (!ref->next) rec_len = (u16)(BLOCK_SIZE - off);
        write_dirent(buf, off, child->ino, rec_len, child->name,
                     child->is_dir ? EXT2_FT_DIR : EXT2_FT_REG_FILE);
        off += rec_len;
    }
}

static void write_file(node_t* file) {
    FILE* f = fopen(file->src_path, "rb");
    if (!f) {
        fprintf(stderr, "mkext2: cannot open '%s'\n", file->src_path);
        exit(1);
    }
    u32 remaining = file->size;
    for (u32 i = 0; i < file->blocks_needed; i++) {
        u8* dst = block_ptr(file->data_blocks[i]);
        memset(dst, 0, BLOCK_SIZE);
        u32 to_read = remaining < BLOCK_SIZE ? remaining : BLOCK_SIZE;
        if (to_read > 0 && fread(dst, 1, to_read, f) != to_read) {
            fclose(f);
            die("file read failed");
        }
        remaining -= to_read;
    }
    fclose(f);
}

static void write_data(node_t* node) {
    if (node->is_dir) {
        write_directory(node);
        for (child_ref_t* ref = node->children; ref; ref = ref->next) {
            write_data(ref->node);
        }
    } else {
        write_file(node);
    }
}

static u32 alloc_pointer_block(void) {
    static u32 next_ptr = 0;
    if (next_ptr == 0) next_ptr = FIRST_DATA_BLOCK;
    while (next_ptr < TOTAL_BLOCKS) {
        int is_data = 0;
        /* Pointer blocks are the unclaimed holes after each file's data span. */
        u8* bitmap = block_ptr(BLOCK_BITMAP_BLOCK);
        if (!((bitmap[next_ptr / 8u] >> (next_ptr % 8u)) & 1u)) {
            bitmap[next_ptr / 8u] = (u8)(bitmap[next_ptr / 8u] | (1u << (next_ptr % 8u)));
            memset(block_ptr(next_ptr), 0, BLOCK_SIZE);
            return next_ptr++;
        }
        next_ptr += is_data ? 1u : 1u;
    }
    die("out of pointer blocks");
    return 0;
}

static void fill_inode_blocks(u8* inode, node_t* node) {
    u32 data_blocks = node->blocks_needed;
    u32 idx = 0;
    for (; idx < data_blocks && idx < 12u; idx++) {
        put_u32(inode, 40u + idx * 4u, node->data_blocks[idx]);
    }
    if (idx >= data_blocks) return;

    u32 single = alloc_pointer_block();
    put_u32(inode, 40u + 12u * 4u, single);
    u8* single_buf = block_ptr(single);
    for (u32 p = 0; idx < data_blocks && p < PTRS_PER_BLOCK; p++, idx++) {
        put_u32(single_buf, p * 4u, node->data_blocks[idx]);
    }
    if (idx >= data_blocks) return;

    u32 dbl = alloc_pointer_block();
    put_u32(inode, 40u + 13u * 4u, dbl);
    u8* dbl_buf = block_ptr(dbl);
    for (u32 outer = 0; idx < data_blocks && outer < PTRS_PER_BLOCK; outer++) {
        u32 indirect = alloc_pointer_block();
        put_u32(dbl_buf, outer * 4u, indirect);
        u8* indirect_buf = block_ptr(indirect);
        for (u32 inner = 0; idx < data_blocks && inner < PTRS_PER_BLOCK; inner++, idx++) {
            put_u32(indirect_buf, inner * 4u, node->data_blocks[idx]);
        }
    }
}

static u32 dir_link_count(node_t* dir) {
    u32 links = 2;
    for (child_ref_t* ref = dir->children; ref; ref = ref->next) {
        if (ref->node->is_dir) links++;
    }
    return links;
}

static void write_inode(node_t* node) {
    u8* ino = inode_ptr(node->ino);
    memset(ino, 0, INODE_SIZE);
    put_u16(ino, 0, (u16)((node->is_dir ? EXT2_S_IFDIR | 0755u : EXT2_S_IFREG | 0644u)));
    put_u32(ino, 4, node->is_dir ? node->blocks_needed * BLOCK_SIZE : node->size);
    put_u16(ino, 26, (u16)(node->is_dir ? dir_link_count(node) : 1u));
    put_u32(ino, 28, (node->blocks_needed + pointer_blocks_for(node->blocks_needed)) * (BLOCK_SIZE / 512u));
    fill_inode_blocks(ino, node);
}

static void write_inodes(node_t* node) {
    write_inode(node);
    for (child_ref_t* ref = node->children; ref; ref = ref->next) {
        write_inodes(ref->node);
    }
}

static void mark_node_blocks(node_t* node) {
    u8* bitmap = block_ptr(BLOCK_BITMAP_BLOCK);
    for (u32 i = 0; i < node->blocks_needed; i++) {
        bit_set(bitmap, node->data_blocks[i]);
    }
    for (child_ref_t* ref = node->children; ref; ref = ref->next) {
        mark_node_blocks(ref->node);
    }
}

static void mark_inode_bits(void) {
    u8* bitmap = block_ptr(INODE_BITMAP_BLOCK);
    for (u32 ino = 1; ino < FIRST_NONRES_INO; ino++) {
        bit_set(bitmap, ino - 1u);
    }
    for (u32 ino = FIRST_NONRES_INO; ino < s_next_ino; ino++) {
        bit_set(bitmap, ino - 1u);
    }
}

static u32 count_free_blocks(void) {
    u8* bitmap = block_ptr(BLOCK_BITMAP_BLOCK);
    u32 free_count = 0;
    for (u32 block = 0; block < TOTAL_BLOCKS; block++) {
        if (!((bitmap[block / 8u] >> (block % 8u)) & 1u)) free_count++;
    }
    return free_count;
}

static u32 count_free_inodes(void) {
    u8* bitmap = block_ptr(INODE_BITMAP_BLOCK);
    u32 free_count = 0;
    for (u32 ino = 1; ino <= TOTAL_INODES; ino++) {
        if (!((bitmap[(ino - 1u) / 8u] >> ((ino - 1u) % 8u)) & 1u)) free_count++;
    }
    return free_count;
}

static void write_superblock(void) {
    u8* sb = block_ptr(0) + 1024u;
    memset(sb, 0, 1024u);
    put_u32(sb, 0, TOTAL_INODES);
    put_u32(sb, 4, TOTAL_BLOCKS);
    put_u32(sb, 8, 0);
    put_u32(sb, 12, count_free_blocks());
    put_u32(sb, 16, count_free_inodes());
    put_u32(sb, 20, 0);
    put_u32(sb, 24, 2);
    put_u32(sb, 28, 2);
    put_u32(sb, 32, TOTAL_BLOCKS);
    put_u32(sb, 36, TOTAL_BLOCKS);
    put_u32(sb, 40, TOTAL_INODES);
    put_u16(sb, 56, SUPER_MAGIC);
    put_u16(sb, 58, 1);
    put_u32(sb, 76, 1);
    put_u32(sb, 84, FIRST_NONRES_INO);
    put_u16(sb, 88, INODE_SIZE);
    put_u32(sb, 92, 2);
    put_u32(sb, 96, 0x2);
    put_u32(sb, 100, 0x2);
}

static void write_group_desc(void) {
    u8* gd = block_ptr(GROUP_DESC_BLOCK);
    memset(gd, 0, BLOCK_SIZE);
    put_u32(gd, 0, BLOCK_BITMAP_BLOCK);
    put_u32(gd, 4, INODE_BITMAP_BLOCK);
    put_u32(gd, 8, INODE_TABLE_BLOCK);
    put_u16(gd, 12, (u16)count_free_blocks());
    put_u16(gd, 14, (u16)count_free_inodes());
}

static void prepare_metadata(node_t* root) {
    memset(s_img, 0, TOTAL_BYTES);
    u8* block_bitmap = block_ptr(BLOCK_BITMAP_BLOCK);
    for (u32 block = 0; block < FIRST_DATA_BLOCK; block++) {
        bit_set(block_bitmap, block);
    }
    mark_node_blocks(root);
    mark_inode_bits();
    write_data(root);
    write_inodes(root);
    write_superblock();
    write_group_desc();
}

static void verify_image(node_t* root) {
    u8* sb = block_ptr(0) + 1024u;
    if (get_u32(sb, 0) != TOTAL_INODES ||
        get_u32(sb, 4) != TOTAL_BLOCKS ||
        sb[56] != 0x53 || sb[57] != 0xEF) {
        die("superblock verification failed");
    }
    u8* gd = block_ptr(GROUP_DESC_BLOCK);
    if (get_u32(gd, 0) != BLOCK_BITMAP_BLOCK ||
        get_u32(gd, 4) != INODE_BITMAP_BLOCK ||
        get_u32(gd, 8) != INODE_TABLE_BLOCK) {
        die("group descriptor verification failed");
    }
    u8* root_inode = inode_ptr(ROOT_INO);
    if ((root_inode[0] & 0xF0u) == 0 || get_u32(root_inode, 40) != root->data_blocks[0]) {
        die("root inode verification failed");
    }
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: mkext2 output.img [dest=]source ... dir/ ...\n");
        return 1;
    }
    if (argc - 2 > MAX_SPECS) die("too many entries");

    node_t* root = node_create("", 1, 0);
    for (int i = 2; i < argc; i++) {
        char dest[PATH_MAX_CHARS];
        int is_dir = 0;
        const char* src = parse_spec(argv[i], dest, sizeof(dest), &is_dir);
        if (is_dir) {
            add_directory_entry(root, dest);
        } else {
            if (!src) die("invalid destination path");
            add_file_entry(root, dest, src);
        }
    }

    compute_layout(root);
    assign_inodes(root);
    assign_blocks(root);

    s_img = (u8*)calloc(1, TOTAL_BYTES);
    if (!s_img) die("out of memory");
    prepare_metadata(root);
    verify_image(root);

    FILE* out = fopen(argv[1], "wb");
    if (!out) {
        fprintf(stderr, "mkext2: cannot open output '%s'\n", argv[1]);
        return 1;
    }
    if (fwrite(s_img, 1, TOTAL_BYTES, out) != TOTAL_BYTES) die("write failed");
    fclose(out);

    fprintf(stdout, "ext2: %s  %u block volume\n", argv[1], TOTAL_BLOCKS);
    free(s_img);
    return 0;
}
