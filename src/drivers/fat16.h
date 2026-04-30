#ifndef FAT16_H
#define FAT16_H

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/*
 * fat16.h — FAT16 filesystem driver
 *
 * The FAT16 partition start LBA is stored at byte offset 504 in the
 * disk boot sector (sector 0) and read at runtime by fat16_init().
 * This avoids any compile-time dependency on a generated header.
 *
 * Volume geometry matches mkfat16.c exactly:
 *
 *   Sector   0        Boot sector (BPB)
 *   Sectors  1–3      Reserved
 *   Sectors  4–35     FAT 1  (32 sectors)
 *   Sectors 36–67     FAT 2  (mirror, not used for reads)
 *   Sectors 68–99     Root directory (512 entries × 32 bytes)
 *   Sectors 100+      Data region  (cluster 2 = sectors 100–103, etc.)
 *
 * Maximum file size accepted: 256 KB (enough for any ELF in this system).
 */

#define FAT16_MAX_FILE_BYTES  (256u * 1024u)

/*
 * Byte offset within the disk boot sector (sector 0) where the Makefile
 * patches in the FAT16 partition start LBA as a little-endian u32.
 * Must not overlap the BPB (bytes 0–61) or the boot signature (510–511).
 */
#define FAT16_LBA_OFFSET_IN_SECTOR0  504u

/*
 * fat16_init()
 *
 * Reads sector 0 via ATA, extracts FAT16_LBA from offset 504, then
 * reads the FAT16 boot sector and validates the volume geometry.
 *
 * Must be called after ata_init(), before any other fat16 call.
 * Returns 1 on success, 0 on failure.
 */
int fat16_init(void);

/*
 * fat16_ls()
 *
 * Print all non-empty root directory entries to the terminal.
 */
void fat16_ls(void);

/*
 * fat16_stat(name, out_size)
 *
 * Check whether a file exists in the root directory (case-insensitive 8.3)
 * and return its size.  Does not load the file data.
 *
 * Returns 1 if found (and sets *out_size), 0 if not found or on error.
 * Used by SYS_OPEN to validate a file before recording it in the fd table.
 */
int fat16_stat(const char* name, u32* out_size);

/*
 * fat16_load(name, out_size)
 *
 * Find a file by name in the root directory (case-insensitive 8.3),
 * load its entire contents into a static kernel buffer, and return a
 * pointer to that buffer.  Sets *out_size to the file size in bytes.
 *
 * Returns 0 on failure (not found, too large, ATA error).
 * The caller must not free the returned buffer; it is reused across
 * fat16_load() calls.
 */
const u8* fat16_load(const char* name, u32* out_size);

/*
 * fat16_write(name, data, size)
 *
 * Create or overwrite a root-directory file with the provided data.
 * The file is stored in 8.3 form and is limited to the root directory;
 * subdirectories and long filenames are not supported.
 *
 * Returns 1 on success, 0 on failure.
 */
int fat16_write(const char* name, const u8* data, u32 size);

#endif /* FAT16_H */
