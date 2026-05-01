#ifndef FAT16_H
#define FAT16_H

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

/*
 * fat16.h — FAT16 filesystem driver
 *
 * The FAT16 partition start LBA is stored in the MBR partition table
 * entry for the FAT16 volume and read at runtime by fat16_init().
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
 * fat16_init()
 *
 * Reads sector 0 via ATA, extracts FAT16_LBA from the FAT16 partition
 * entry in the MBR partition table, then reads the FAT16 boot sector
 * and validates the volume geometry.
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
 * fat16_ls_path(path)
 *
 * Print the entries in the named directory.  Passing 0 or "" lists the
 * root directory.
 */
void fat16_ls_path(const char* path);

/*
 * fat16_ls_path_filtered(path, pattern)
 *
 * Print the entries in the named directory, filtered by a wildcard
 * pattern such as "*.elf" or "TCC*.C".  Passing 0 for pattern lists
 * all entries.  Passing 0 or "" for path lists the root directory.
 */
void fat16_ls_path_filtered(const char* path, const char* pattern);

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

/*
 * fat16_write_path(path, data, size)
 *
 * Create or overwrite a file at a full path.  The destination may be a
 * nested directory path, unlike fat16_write() which preserves the
 * historical root-directory-only behavior.
 *
 * Returns 1 on success, 0 on failure.
 */
int fat16_write_path(const char* path, const u8* data, u32 size);

/*
 * fat16_mkdir(path)
 *
 * Create a new empty directory at `path`.
 * The target must not already exist.
 *
 * Returns 1 on success, 0 on failure.
 */
int fat16_mkdir(const char* path);

/*
 * fat16_rmdir(path)
 *
 * Remove an existing empty directory at `path`.
 * The root directory cannot be removed.
 *
 * Returns 1 on success, 0 on failure.
 */
int fat16_rmdir(const char* path);

/*
 * fat16_rm(path)
 *
 * Remove an existing file at `path`.
 * The target must be a file, not a directory.
 *
 * Returns 1 on success, 0 on failure.
 */
int fat16_rm(const char* path);

/*
 * fat16_is_dir(path)
 *
 * Check whether the named path resolves to a directory.  The root
 * directory counts as a directory.
 *
 * Returns 1 if the path is a directory, 0 otherwise.
 */
int fat16_is_dir(const char* path);

/*
 * fat16_dirent_at(path, index, out_name, out_name_size, out_size, out_is_dir)
 *
 * Return the Nth entry from the named directory as a display name and
 * metadata tuple.  Indexing is zero-based and excludes "." / "..".
 * Returns 1 if the entry exists, 0 otherwise.
 */
int fat16_dirent_at(const char* path,
                    u32 index,
                    char* out_name,
                    u32 out_name_size,
                    u32* out_size,
                    int* out_is_dir);

/*
 * fat16_copy(src, dst)
 *
 * Copy a file from src to dst.  If dst names an existing directory, the
 * source leaf name is copied into that directory.
 *
 * Returns 1 on success, 0 on failure.
 */
int fat16_copy(const char* src, const char* dst);

/*
 * fat16_move(src, dst)
 *
 * Move or rename a file or directory.  If dst names an existing
 * directory, the source leaf name is moved into that directory.
 *
 * Returns 1 on success, 0 on failure.
 */
int fat16_move(const char* src, const char* dst);

#endif /* FAT16_H */
