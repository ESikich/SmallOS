/*
 * mkfat16.c — SmallOS FAT16 image builder
 *
 * Usage:
 *   mkfat16 output.img file1.elf file2.elf ...
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
 * Files are stored contiguously starting at cluster 2, four sectors
 * (2048 bytes) per cluster.  Filenames are stored in 8.3 format.
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

/* ------------------------------------------------------------------ */
/* Directory entry                                                     */
/* ------------------------------------------------------------------ */

static void write_dirent(u8* entry, const u8 name83[11],
                         u16 start_cluster, u32 file_size) {
    memset(entry, 0, 32);
    memcpy(entry,      name83, 11);
    entry[11] = 0x20;                       /* attribute: archive */
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
        fprintf(stderr, "Usage: mkfat16 output.img [file.elf ...]\n");
        return 1;
    }

    const char* output_path = argv[1];
    int nfiles = argc - 2;
    if (nfiles > MAX_FILES) die("too many files (max 64)");

    /* Measure files */
    const char* paths[MAX_FILES];
    u32         sizes[MAX_FILES];
    for (int i = 0; i < nfiles; i++) {
        paths[i] = argv[2 + i];
        sizes[i] = measure_file(paths[i]);
    }

    /* Assign clusters */
    fat_init();
    u32 file_cluster[MAX_FILES];
    u32 next_cluster = FIRST_CLUSTER;
    u32 max_data_clusters = (TOTAL_SECTORS - DATA_START) / SECTORS_PER_CLUSTER;

    for (int i = 0; i < nfiles; i++) {
        u32 clusters = (sizes[i] + CLUSTER_BYTES - 1) / CLUSTER_BYTES;
        if (clusters == 0) clusters = 1;
        if (next_cluster + clusters > FIRST_CLUSTER + max_data_clusters) {
            fprintf(stderr, "mkfat16: filesystem full\n");
            return 1;
        }
        file_cluster[i] = next_cluster;
        fat_alloc_chain(next_cluster, clusters);
        next_cluster += clusters;
    }

    /* Open output */
    FILE* out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "mkfat16: cannot open output '%s'\n", output_path);
        return 1;
    }

    u8* zbuf = calloc(CLUSTER_BYTES, 1);   /* general zero / scratch buffer */
    if (!zbuf) die("out of memory");

    /* --- Sector 0: boot sector --- */
    u8 boot[SECTOR_SIZE];
    build_boot_sector(boot);
    if (fwrite(boot, 1, SECTOR_SIZE, out) != SECTOR_SIZE) die("write failed");

    /* --- Sectors 1–3: reserved (zero) --- */
    for (u32 s = 1; s < RESERVED_SECTORS; s++) {
        memset(zbuf, 0, SECTOR_SIZE);
        if (fwrite(zbuf, 1, SECTOR_SIZE, out) != SECTOR_SIZE) die("write failed");
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
    u8* root = calloc(ROOT_DIR_SECTORS * SECTOR_SIZE, 1);
    if (!root) die("out of memory");

    for (int i = 0; i < nfiles; i++) {
        u8 name83[11];
        to_83(paths[i], name83);
        write_dirent(root + i * 32, name83, (u16)file_cluster[i], sizes[i]);

        /* Pretty-print 8.3 name for build log */
        char disp[13];
        int di = 0;
        for (int j = 0; j < 8 && name83[j] != ' '; j++) disp[di++] = name83[j];
        disp[di++] = '.';
        for (int j = 0; j < 3 && name83[8+j] != ' '; j++) disp[di++] = name83[8+j];
        disp[di] = '\0';
        fprintf(stdout, "  %-12s  cluster %4u  %7u bytes  (%s)\n",
                disp, file_cluster[i], sizes[i], paths[i]);
    }

    if (fwrite(root, 1, ROOT_DIR_SECTORS * SECTOR_SIZE, out)
            != ROOT_DIR_SECTORS * SECTOR_SIZE) die("write failed");
    free(root);

    /* --- Data region --- */
    u8* cbuf = calloc(CLUSTER_BYTES, 1);
    if (!cbuf) die("out of memory");

    u32 current_sector = DATA_START;

    for (int i = 0; i < nfiles; i++) {
        u32 clusters = (sizes[i] + CLUSTER_BYTES - 1) / CLUSTER_BYTES;
        if (clusters == 0) clusters = 1;

        FILE* f = fopen(paths[i], "rb");
        if (!f) {
            fprintf(stderr, "mkfat16: cannot open '%s'\n", paths[i]);
            fclose(out);
            return 1;
        }

        u32 remaining = sizes[i];
        for (u32 c = 0; c < clusters; c++) {
            memset(cbuf, 0, CLUSTER_BYTES);
            u32 to_read = remaining < CLUSTER_BYTES ? remaining : CLUSTER_BYTES;
            if (to_read > 0) {
                size_t got = fread(cbuf, 1, to_read, f);
                if (got != to_read) {
                    fprintf(stderr, "mkfat16: read error on '%s'\n", paths[i]);
                    fclose(f); fclose(out); return 1;
                }
                remaining -= (u32)got;
            }
            if (fwrite(cbuf, 1, CLUSTER_BYTES, out) != CLUSTER_BYTES)
                die("write failed");
            current_sector += SECTORS_PER_CLUSTER;
        }
        fclose(f);
    }

    /* Pad remaining sectors to reach TOTAL_SECTORS */
    memset(cbuf, 0, CLUSTER_BYTES);
    while (current_sector < TOTAL_SECTORS) {
        u32 left = TOTAL_SECTORS - current_sector;
        u32 write_sectors = left < SECTORS_PER_CLUSTER ? left : SECTORS_PER_CLUSTER;
        u32 write_bytes = write_sectors * SECTOR_SIZE;
        if (fwrite(cbuf, 1, write_bytes, out) != write_bytes) die("write failed");
        current_sector += write_sectors;
    }

    free(cbuf);
    free(zbuf);
    fclose(out);

    fprintf(stdout, "fat16: %s  %d file(s)  %u sectors (%u KB)\n",
            output_path, nfiles, TOTAL_SECTORS, TOTAL_SECTORS / 2);
    return 0;
}