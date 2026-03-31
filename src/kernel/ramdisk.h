#ifndef RAMDISK_H
#define RAMDISK_H

typedef unsigned int  u32;
typedef unsigned char u8;

#define RAMDISK_MAGIC    0x52445349u   /* 'RDSI' little-endian */
#define RAMDISK_NAME_LEN 32

/*
 * Ramdisk archive layout (all fields little-endian):
 *
 *   [rd_header_t]
 *   [rd_entry_t × header.count]
 *   [raw file data, concatenated]
 *
 * rd_entry_t.offset is a byte offset from the start of the archive.
 * rd_entry_t.size   is the byte length of the file data.
 */

typedef struct {
    u32 magic;
    u32 count;
} __attribute__((packed)) rd_header_t;

typedef struct {
    char name[RAMDISK_NAME_LEN];
    u32  offset;
    u32  size;
} __attribute__((packed)) rd_entry_t;

/*
 * ramdisk_init(base)
 *
 * Register the ramdisk loaded at physical address `base`.
 * Validates the magic number.  Must be called before ramdisk_find().
 * Returns 1 on success, 0 if the magic is wrong.
 */
int ramdisk_init(u32 base);

/*
 * ramdisk_find(name, out_data, out_size)
 *
 * Look up a file by name.  On success, sets *out_data to a pointer to
 * the file's raw bytes (within the ramdisk image) and *out_size to its
 * length.  Returns 1 on success, 0 if not found.
 */
int ramdisk_find(const char* name, const u8** out_data, u32* out_size);

#endif