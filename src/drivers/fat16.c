#include "fat16.h"
#include "ata.h"
#include "../drivers/terminal.h"
#include "../kernel/klib.h"

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
#define FAT16_MAX_FILE_CLUSTERS (FAT16_MAX_FILE_BYTES / CLUSTER_BYTES)

/* ------------------------------------------------------------------ */
/* Directory entry field offsets                                        */
/* ------------------------------------------------------------------ */

#define DIR_NAME          0    /* 11 bytes */
#define DIR_ATTR          11
#define DIR_FIRST_CLUSTER 26   /* u16 */
#define DIR_FILE_SIZE     28   /* u32 */

#define ATTR_VOLUME_ID    0x08
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

/*
 * Static ELF load buffer — reused across fat16_load() calls.
 * Lives in BSS (zeroed at boot, never freed).  Since ELF programs run
 * sequentially (one foreground process at a time), there is no aliasing
 * risk.  The caller (elf_run_image) copies all data into PMM frames
 * before returning, so the buffer is safe to reuse on the next call.
 *
 * A separate static cluster scratch buffer is used to avoid a 2048-byte
 * allocation on the kernel stack inside fat16_load().
 */
static u8 s_load_buf[FAT16_MAX_FILE_BYTES];
static u8 s_cluster_buf[CLUSTER_BYTES];

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

static void write_dirent(u8* entry, const u8 name83[11], u16 start_cluster, u32 file_size) {
    k_memset(entry, 0, 32);
    k_memcpy(entry, name83, 11);
    entry[11] = 0x20;
    write_u16_le(entry, DIR_FIRST_CLUSTER, start_cluster);
    write_u32_le(entry, DIR_FILE_SIZE, file_size);
}

static int load_fat_and_root(void) {
    if (!ata_read_sectors(abs_lba(FAT1_REL_SECTOR), FAT_SECTORS, s_fat_buf)) {
        terminal_puts("fat16: FAT read error\n");
        return 0;
    }

    if (!ata_read_sectors(abs_lba(ROOT_REL_SECTOR), ROOT_DIR_SECTORS, s_root_buf)) {
        terminal_puts("fat16: root dir read error\n");
        return 0;
    }

    return 1;
}

static int write_fat_and_root(void) {
    if (!ata_write_sectors(abs_lba(FAT1_REL_SECTOR), FAT_SECTORS, s_fat_buf)) {
        terminal_puts("fat16: FAT write error\n");
        return 0;
    }
    if (!ata_write_sectors(abs_lba(FAT2_REL_SECTOR), FAT_SECTORS, s_fat_buf)) {
        terminal_puts("fat16: FAT mirror write error\n");
        return 0;
    }
    if (!ata_write_sectors(abs_lba(ROOT_REL_SECTOR), ROOT_DIR_SECTORS, s_root_buf)) {
        terminal_puts("fat16: root dir write error\n");
        return 0;
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

static int write_data_clusters(const u32* chain, u32 clusters, const u8* data, u32 size) {
    u8 sector[SECTOR_SIZE];
    u32 offset = 0;
    u32 remaining = size;

    for (u32 i = 0; i < clusters; i++) {
        for (u32 s = 0; s < SECTORS_PER_CLUSTER; s++) {
            k_memset(sector, 0, SECTOR_SIZE);
            u32 chunk = remaining < SECTOR_SIZE ? remaining : SECTOR_SIZE;
            if (chunk > 0) {
                k_memcpy(sector, data + offset, chunk);
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

void fat16_ls(void) {
    if (!s_initialised) { terminal_puts("fat16: not initialised\n"); return; }

    terminal_puts("fat16 root directory:\n");

    for (u32 s = 0; s < ROOT_DIR_SECTORS; s++) {
        if (!ata_read_sectors(abs_lba(ROOT_REL_SECTOR + s), 1, s_sector)) {
            terminal_puts("fat16: read error\n");
            return;
        }

        for (u32 e = 0; e < SECTOR_SIZE / 32u; e++) {
            const u8* entry = s_sector + e * 32u;

            if (entry[DIR_NAME] == 0x00) return;
            if (entry[DIR_NAME] == 0xE5) continue;

            u8 attr = entry[DIR_ATTR];
            if (attr == ATTR_LONG_NAME)  continue;
            if (attr & ATTR_VOLUME_ID)   continue;

            char name[13];
            int  ni = 0;
            for (int j = 0; j < 8 && entry[DIR_NAME + j] != ' '; j++)
                name[ni++] = (char)entry[DIR_NAME + j];
            name[ni++] = '.';
            for (int j = 0; j < 3 && entry[DIR_NAME + 8 + j] != ' '; j++)
                name[ni++] = (char)entry[DIR_NAME + 8 + j];
            name[ni] = '\0';

            u16 cluster   = read_u16_le(entry, DIR_FIRST_CLUSTER);
            u32 file_size = read_u32_le(entry, DIR_FILE_SIZE);

            terminal_puts("  ");
            terminal_puts(name);
            terminal_puts("  ");
            terminal_put_uint(file_size);
            terminal_puts(" bytes  cluster ");
            terminal_put_uint(cluster);
            terminal_putc('\n');
        }
    }
}

int fat16_stat(const char* name, u32* out_size) {
    if (!s_initialised) return 0;

    for (u32 s = 0; s < ROOT_DIR_SECTORS; s++) {
        if (!ata_read_sectors(abs_lba(ROOT_REL_SECTOR + s), 1, s_sector))
            return 0;

        for (u32 e = 0; e < SECTOR_SIZE / 32u; e++) {
            const u8* entry = s_sector + e * 32u;

            if (entry[DIR_NAME] == 0x00) return 0;
            if (entry[DIR_NAME] == 0xE5) continue;

            u8 attr = entry[DIR_ATTR];
            if (attr == ATTR_LONG_NAME) continue;
            if (attr & ATTR_VOLUME_ID)  continue;

            if (match_83(entry + DIR_NAME, name)) {
                *out_size = read_u32_le(entry, DIR_FILE_SIZE);
                return 1;
            }
        }
    }
    return 0;
}

const u8* fat16_load(const char* name, u32* out_size) {
    if (!s_initialised) { terminal_puts("fat16: not initialised\n"); return 0; }

    /* Search root directory */
    u16 start_cluster = 0;
    u32 file_size     = 0;
    int found         = 0;

    for (u32 s = 0; s < ROOT_DIR_SECTORS && !found; s++) {
        if (!ata_read_sectors(abs_lba(ROOT_REL_SECTOR + s), 1, s_sector)) {
            terminal_puts("fat16: dir read error\n");
            return 0;
        }

        for (u32 e = 0; e < SECTOR_SIZE / 32u && !found; e++) {
            const u8* entry = s_sector + e * 32u;

            if (entry[DIR_NAME] == 0x00) goto search_done;
            if (entry[DIR_NAME] == 0xE5) continue;

            u8 attr = entry[DIR_ATTR];
            if (attr == ATTR_LONG_NAME) continue;
            if (attr & ATTR_VOLUME_ID)  continue;

            if (match_83(entry + DIR_NAME, name)) {
                start_cluster = read_u16_le(entry, DIR_FIRST_CLUSTER);
                file_size     = read_u32_le(entry, DIR_FILE_SIZE);
                found = 1;
            }
        }
    }

search_done:
    if (!found) {
        terminal_puts("fat16: not found: ");
        terminal_puts(name);
        terminal_putc('\n');
        return 0;
    }

    if (file_size == 0) {
        terminal_puts("fat16: file is empty\n");
        return 0;
    }

    if (file_size > FAT16_MAX_FILE_BYTES) {
        terminal_puts("fat16: file too large\n");
        return 0;
    }

    /* Use the static load buffer — safe since ELFs run sequentially */
    u8* buf = s_load_buf;

    /* Load cluster chain into buffer */
    u32 bytes_remaining = file_size;
    u32 buf_offset      = 0;
    u32 cluster         = (u32)start_cluster;

    while (cluster >= FIRST_CLUSTER && cluster < FAT16_EOC_MIN) {
        u32 rel_sector = cluster_to_rel_sector(cluster);

        for (u32 s = 0; s < SECTORS_PER_CLUSTER; s++) {
            if (!ata_read_sectors(abs_lba(rel_sector + s), 1,
                                  s_cluster_buf + s * SECTOR_SIZE)) {
                terminal_puts("fat16: cluster read error\n");
                return 0;
            }
        }

        u32 to_copy = bytes_remaining < CLUSTER_BYTES
                    ? bytes_remaining : CLUSTER_BYTES;

        for (u32 i = 0; i < to_copy; i++)
            buf[buf_offset + i] = s_cluster_buf[i];

        buf_offset      += to_copy;
        bytes_remaining -= to_copy;

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

int fat16_write(const char* name, const u8* data, u32 size) {
    if (!s_initialised) {
        terminal_puts("fat16: not initialised\n");
        return 0;
    }

    if (!name || (!data && size > 0)) {
        return 0;
    }

    if (size > FAT16_MAX_FILE_BYTES) {
        terminal_puts("fat16: file too large\n");
        return 0;
    }

    u8 name83[11];
    to_83(name, name83);
    if (name83[0] == ' ') {
        return 0;
    }

    if (!load_fat_and_root()) {
        return 0;
    }

    u16* fat = (u16*)s_fat_buf;
    int existing_entry = -1;
    int free_entry = -1;
    u16 old_start_cluster = 0;
    u32 old_chain[FAT16_MAX_FILE_CLUSTERS];
    u32 old_clusters = 0;

    for (u32 e = 0; e < ROOT_ENTRY_COUNT; e++) {
        u8* entry = s_root_buf + e * 32u;

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

        if (existing_entry < 0 && match_83(entry + DIR_NAME, name)) {
            existing_entry = (int)e;
            old_start_cluster = read_u16_le(entry, DIR_FIRST_CLUSTER);
        }
    }

    if (existing_entry < 0 && free_entry < 0) {
        terminal_puts("fat16: root directory full\n");
        return 0;
    }

    u32 clusters_needed = 0;
    if (size > 0) {
        clusters_needed = (size + CLUSTER_BYTES - 1u) / CLUSTER_BYTES;
        if (clusters_needed > FAT16_MAX_FILE_CLUSTERS) {
            terminal_puts("fat16: file too large\n");
            return 0;
        }
    }

    if (existing_entry >= 0 && old_start_cluster >= FIRST_CLUSTER) {
        u32 cluster = (u32)old_start_cluster;
        while (cluster >= FIRST_CLUSTER &&
               cluster < FAT16_EOC_MIN &&
               old_clusters < FAT16_MAX_FILE_CLUSTERS) {
            old_chain[old_clusters++] = cluster;
            u16 next = fat[cluster];
            fat[cluster] = 0;
            if (next >= FAT16_EOC_MIN) {
                break;
            }
            cluster = next;
        }
    }

    u32 chain[FAT16_MAX_FILE_CLUSTERS];
    if (clusters_needed > 0 && !find_free_chain(fat, clusters_needed, chain)) {
        terminal_puts("fat16: filesystem full\n");

        if (old_clusters > 0) {
            for (u32 i = 0; i < old_clusters; i++) {
                u16 next = (i + 1 < old_clusters) ? (u16)old_chain[i + 1] : FAT16_EOC_MIN;
                fat[old_chain[i]] = next;
            }
        }

        return 0;
    }

    if (clusters_needed > 0 && !write_data_clusters(chain, clusters_needed, data, size)) {
        return 0;
    }

    if (clusters_needed > 0) {
        for (u32 i = 0; i < clusters_needed; i++) {
            u16 next = (i + 1 < clusters_needed) ? (u16)chain[i + 1] : FAT16_EOC_MIN;
            fat[chain[i]] = next;
        }
    }

    u8* entry = 0;
    int entry_idx = (existing_entry >= 0) ? existing_entry : free_entry;
    if (entry_idx < 0) {
        terminal_puts("fat16: root directory full\n");
        return 0;
    }

    entry = s_root_buf + (u32)entry_idx * 32u;
    write_dirent(entry, name83,
                 clusters_needed > 0 ? (u16)chain[0] : 0,
                 size);

    if (!write_fat_and_root()) {
        return 0;
    }

    return 1;
}
