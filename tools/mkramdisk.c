/*
 * mkramdisk.c — SmallOS ramdisk builder
 *
 * Usage:
 *   mkramdisk output.rd name1:file1.elf name2:file2.elf ...
 *
 * Produces a flat binary archive:
 *
 *   [header]
 *       magic       4 bytes   0x52445349  ('RDSI', little-endian)
 *       count       4 bytes   number of entries
 *
 *   [entry × count]
 *       name       32 bytes   null-terminated
 *       offset      4 bytes   byte offset of file data from archive start
 *       size        4 bytes   byte size of file data
 *
 *   [file data]
 *       raw bytes of each file, concatenated
 *
 * Build:
 *   gcc -o tools/mkramdisk tools/mkramdisk.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RAMDISK_MAGIC    0x52445349u
#define RAMDISK_NAME_LEN 32
#define MAX_FILES        64

typedef unsigned int u32;

/* Packed structs — written directly to the output file. */
#pragma pack(push, 1)
typedef struct {
    u32 magic;
    u32 count;
} rd_header_t;

typedef struct {
    char name[RAMDISK_NAME_LEN];
    u32  offset;
    u32  size;
} rd_entry_t;
#pragma pack(pop)

typedef struct {
    char  name[RAMDISK_NAME_LEN];
    char* path;
    u32   size;
} file_info_t;

static void die(const char* msg) {
    fprintf(stderr, "mkramdisk: %s\n", msg);
    exit(1);
}

static u32 file_size(FILE* f) {
    if (fseek(f, 0, SEEK_END) != 0) die("fseek failed");
    long sz = ftell(f);
    if (sz < 0) die("ftell failed");
    rewind(f);
    return (u32)sz;
}

static void write_u32_le(FILE* f, u32 val) {
    /* Write in little-endian order regardless of host endianness. */
    unsigned char b[4];
    b[0] = (unsigned char)(val);
    b[1] = (unsigned char)(val >> 8);
    b[2] = (unsigned char)(val >> 16);
    b[3] = (unsigned char)(val >> 24);
    if (fwrite(b, 1, 4, f) != 4) die("write failed");
}

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "Usage: mkramdisk output.rd name1:file1.elf ...\n");
        return 1;
    }

    const char* output_path = argv[1];
    int count = argc - 2;

    if (count > MAX_FILES) die("too many files (max 64)");

    file_info_t files[MAX_FILES];
    memset(files, 0, sizeof(files));

    /* Parse name:path arguments and measure file sizes. */
    for (int i = 0; i < count; i++) {
        char* arg = argv[2 + i];
        char* colon = strchr(arg, ':');
        if (!colon) {
            fprintf(stderr, "mkramdisk: expected name:path, got '%s'\n", arg);
            return 1;
        }

        size_t name_len = (size_t)(colon - arg);
        if (name_len == 0) die("empty name");
        if (name_len >= RAMDISK_NAME_LEN) {
            fprintf(stderr, "mkramdisk: name too long (max %d chars): %s\n",
                    RAMDISK_NAME_LEN - 1, arg);
            return 1;
        }

        memcpy(files[i].name, arg, name_len);
        files[i].name[name_len] = '\0';
        files[i].path = colon + 1;

        FILE* f = fopen(files[i].path, "rb");
        if (!f) {
            fprintf(stderr, "mkramdisk: cannot open '%s'\n", files[i].path);
            return 1;
        }
        files[i].size = file_size(f);
        fclose(f);
    }

    /* Calculate data offsets.
     * Data region starts immediately after header + entry table. */
    u32 header_size = (u32)sizeof(rd_header_t);
    u32 entry_size  = (u32)sizeof(rd_entry_t);
    u32 data_start  = header_size + entry_size * (u32)count;
    u32 offset      = data_start;

    /* Open output file. */
    FILE* out = fopen(output_path, "wb");
    if (!out) {
        fprintf(stderr, "mkramdisk: cannot open output '%s'\n", output_path);
        return 1;
    }

    /* Write header. */
    write_u32_le(out, RAMDISK_MAGIC);
    write_u32_le(out, (u32)count);

    /* Write entry table. */
    for (int i = 0; i < count; i++) {
        /* name: 32 bytes, zero-padded */
        char name_buf[RAMDISK_NAME_LEN];
        memset(name_buf, 0, RAMDISK_NAME_LEN);
        strncpy(name_buf, files[i].name, RAMDISK_NAME_LEN - 1);
        if (fwrite(name_buf, 1, RAMDISK_NAME_LEN, out) != RAMDISK_NAME_LEN)
            die("write failed");

        write_u32_le(out, offset);
        write_u32_le(out, files[i].size);

        offset += files[i].size;
    }

    /* Write file data. */
    for (int i = 0; i < count; i++) {
        FILE* f = fopen(files[i].path, "rb");
        if (!f) {
            fprintf(stderr, "mkramdisk: cannot open '%s'\n", files[i].path);
            fclose(out);
            return 1;
        }

        unsigned char buf[4096];
        size_t remaining = files[i].size;
        while (remaining > 0) {
            size_t chunk = remaining < sizeof(buf) ? remaining : sizeof(buf);
            size_t got = fread(buf, 1, chunk, f);
            if (got == 0) {
                fprintf(stderr, "mkramdisk: read error on '%s'\n",
                        files[i].path);
                fclose(f);
                fclose(out);
                return 1;
            }
            if (fwrite(buf, 1, got, out) != got) die("write failed");
            remaining -= got;
        }

        fclose(f);

        fprintf(stdout, "  %-20s  %6u bytes  (%s)\n",
                files[i].name, files[i].size, files[i].path);
    }

    fclose(out);

    /* Compute total size for summary. */
    u32 total = data_start;
    for (int i = 0; i < count; i++) total += files[i].size;

    fprintf(stdout, "ramdisk: %s  %d file(s)  %u bytes\n",
            output_path, count, total);

    return 0;
}