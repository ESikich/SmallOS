#include "fat16.h"
#include "ata.h"
#include "../drivers/terminal.h"

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
#define ROOT_REL_SECTOR   (RESERVED_SECTORS + NUM_FATS * FAT_SECTORS)  /* 68  */
#define DATA_REL_SECTOR   (ROOT_REL_SECTOR + ROOT_DIR_SECTORS)         /* 100 */

#define FIRST_CLUSTER     2u
#define FAT16_EOC_MIN     0xFFF8u

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
     * Step 1: read sector 0 of the whole disk to get the FAT16 LBA.
     * The Makefile patches a little-endian u32 at byte offset 504.
     */
    if (!ata_read_sectors(0, 1, s_sector)) {
        terminal_puts("fat16: cannot read sector 0\n");
        return 0;
    }
    s_fat16_lba = read_u32_le(s_sector, FAT16_LBA_OFFSET_IN_SECTOR0);

    if (s_fat16_lba == 0) {
        terminal_puts("fat16: LBA not patched (zero)\n");
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