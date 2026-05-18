#include "ext2.h"
#include "../drivers/terminal.h"
#include "../kernel/boot_info.h"
#include "../kernel/klib.h"
#include "../kernel/memory.h"
#include "../kernel/paging.h"
#include "../kernel/pmm.h"

#define SECTOR_SIZE          512u
#define EXT2_BLOCK_SIZE      4096u
#define EXT2_SECTORS_PER_BLOCK (EXT2_BLOCK_SIZE / SECTOR_SIZE)
#define EXT2_TOTAL_BLOCKS    4096u
#define EXT2_TOTAL_INODES    512u
#define EXT2_INODE_SIZE      128u
#define EXT2_ROOT_INO        2u
#define EXT2_FIRST_NONRES_INO 11u
#define EXT2_SUPER_MAGIC     0xEF53u

#define EXT2_BLOCK_GROUP_DESC_BLOCK 1u
#define EXT2_BLOCK_BITMAP_BLOCK     2u
#define EXT2_INODE_BITMAP_BLOCK     3u
#define EXT2_INODE_TABLE_BLOCK      4u
#define EXT2_INODE_TABLE_BLOCKS     16u
#define EXT2_FIRST_DATA_BLOCK       20u

#define EXT2_PARTITION_ENTRY_INDEX  1u
#define EXT2_PARTITION_TYPE         0x83u
#define MBR_PARTITION_TABLE_OFFSET  446u
#define MBR_PARTITION_ENTRY_SIZE    16u
#define MBR_PARTITION_TYPE_OFFSET   4u
#define MBR_PARTITION_LBA_OFFSET    8u
#define MBR_PARTITION_SIZE_OFFSET   12u

#define EXT2_S_IFREG 0x8000u
#define EXT2_S_IFDIR 0x4000u
#define EXT2_FT_REG_FILE 1u
#define EXT2_FT_DIR      2u

#define EXT2_N_BLOCKS 15u
#define EXT2_DIRECT_BLOCKS 12u
#define EXT2_PTRS_PER_BLOCK (EXT2_BLOCK_SIZE / 4u)
#define EXT2_WRITE_CHUNK_SIZE (16u * EXT2_BLOCK_SIZE)

#define INODE_MODE      0u
#define INODE_UID       2u
#define INODE_SIZE      4u
#define INODE_ATIME     8u
#define INODE_CTIME     12u
#define INODE_MTIME     16u
#define INODE_DTIME     20u
#define INODE_GID       24u
#define INODE_LINKS     26u
#define INODE_BLOCKS    28u
#define INODE_FLAGS     32u
#define INODE_BLOCK     40u

typedef struct {
    u16 mode;
    u16 uid;
    u16 gid;
    u32 size;
    u16 links_count;
    u32 blocks_512;
    u32 atime;
    u32 ctime;
    u32 mtime;
    u32 block[EXT2_N_BLOCKS];
} ext2_inode_t;

typedef struct {
    u32 inode;
    u16 rec_len;
    u8 name_len;
    u8 file_type;
    const u8* raw_name;
} ext2_dirent_t;

typedef struct {
    u32 parent_ino;
    u32 ino;
    ext2_inode_t inode;
    int has_entry;
} resolved_path_t;

typedef struct {
    u32 parent_ino;
    ext2_inode_t parent;
    char leaf[256];
} create_path_t;

typedef struct {
    const u8* data;
} mem_source_t;

typedef struct {
    u8* data;
} mem_sink_t;

static int s_initialised = 0;
static int s_bitmaps_loaded = 0;
static unsigned int s_bitmap_write_defer_depth = 0;
static int s_block_bitmap_dirty = 0;
static int s_inode_bitmap_dirty = 0;
static u32 s_next_alloc_block = EXT2_FIRST_DATA_BLOCK;
static u32 s_ext2_lba = 0;
static u32 s_ext2_sectors = 0;
static u8* s_ramdisk = 0;
static u32 s_ramdisk_size = 0;
static int s_use_boot_ramdisk = 0;
static block_device_t* s_block_dev = 0;
static u8* s_load_buf = 0;

static u8* s_sector = 0;
static u8* s_block = 0;
static u8* s_block2 = 0;
static u8* s_data_buf = 0;
static u8* s_block_bitmap = 0;
static u8* s_inode_bitmap = 0;

static u16 read_u16_le(const u8* buf, u32 off) {
    return (u16)(buf[off] | ((u16)buf[off + 1u] << 8));
}

static u32 read_u32_le(const u8* buf, u32 off) {
    return (u32)buf[off]
         | ((u32)buf[off + 1u] << 8)
         | ((u32)buf[off + 2u] << 16)
         | ((u32)buf[off + 3u] << 24);
}

static void write_u16_le(u8* buf, u32 off, u16 value) {
    buf[off] = (u8)(value & 0xFFu);
    buf[off + 1u] = (u8)((value >> 8) & 0xFFu);
}

static void write_u32_le(u8* buf, u32 off, u32 value) {
    buf[off] = (u8)(value & 0xFFu);
    buf[off + 1u] = (u8)((value >> 8) & 0xFFu);
    buf[off + 2u] = (u8)((value >> 16) & 0xFFu);
    buf[off + 3u] = (u8)((value >> 24) & 0xFFu);
}

static int ext2_alloc_scratch_buffers(void) {
    u32 sector_phys;
    u32 block_phys;
    u32 block2_phys;
    u32 data_phys;
    u32 block_bitmap_phys;
    u32 inode_bitmap_phys;

    if (s_sector && s_block && s_block2 && s_data_buf &&
        s_block_bitmap && s_inode_bitmap) {
        return 1;
    }

    sector_phys = pmm_alloc_frame();
    block_phys = pmm_alloc_frame();
    block2_phys = pmm_alloc_frame();
    data_phys = pmm_alloc_contiguous_frames(EXT2_WRITE_CHUNK_SIZE / PMM_FRAME_SIZE);
    block_bitmap_phys = pmm_alloc_frame();
    inode_bitmap_phys = pmm_alloc_frame();
    if (!sector_phys || !block_phys || !block2_phys || !data_phys ||
        !block_bitmap_phys || !inode_bitmap_phys) {
        if (sector_phys) pmm_free_frame(sector_phys);
        if (block_phys) pmm_free_frame(block_phys);
        if (block2_phys) pmm_free_frame(block2_phys);
        if (data_phys) {
            pmm_free_contiguous_frames(data_phys,
                                       EXT2_WRITE_CHUNK_SIZE / PMM_FRAME_SIZE);
        }
        if (block_bitmap_phys) pmm_free_frame(block_bitmap_phys);
        if (inode_bitmap_phys) pmm_free_frame(inode_bitmap_phys);
        return 0;
    }

    s_sector = (u8*)paging_phys_to_kernel_virt(sector_phys);
    s_block = (u8*)paging_phys_to_kernel_virt(block_phys);
    s_block2 = (u8*)paging_phys_to_kernel_virt(block2_phys);
    s_data_buf = (u8*)paging_phys_to_kernel_virt(data_phys);
    s_block_bitmap = (u8*)paging_phys_to_kernel_virt(block_bitmap_phys);
    s_inode_bitmap = (u8*)paging_phys_to_kernel_virt(inode_bitmap_phys);
    k_memset(s_sector, 0, PMM_FRAME_SIZE);
    k_memset(s_block, 0, PMM_FRAME_SIZE);
    k_memset(s_block2, 0, PMM_FRAME_SIZE);
    k_memset(s_data_buf, 0, EXT2_WRITE_CHUNK_SIZE);
    k_memset(s_block_bitmap, 0, PMM_FRAME_SIZE);
    k_memset(s_inode_bitmap, 0, PMM_FRAME_SIZE);
    return 1;
}

static int bit_test(const u8* bits, u32 index) {
    return (bits[index / 8u] & (1u << (index % 8u))) != 0;
}

static void bit_set(u8* bits, u32 index) {
    bits[index / 8u] = (u8)(bits[index / 8u] | (u8)(1u << (index % 8u)));
}

static void bit_clear(u8* bits, u32 index) {
    bits[index / 8u] = (u8)(bits[index / 8u] & (u8)~(1u << (index % 8u)));
}

static int is_sep(char c) {
    return c == '/' || c == '\\';
}

static u32 abs_lba_for_block(u32 block) {
    return s_ext2_lba + block * EXT2_SECTORS_PER_BLOCK;
}

static int read_block(u32 block, u8* out) {
    if (s_ramdisk) {
        u32 offset = block * EXT2_BLOCK_SIZE;
        if (offset > s_ramdisk_size || s_ramdisk_size - offset < EXT2_BLOCK_SIZE) {
            return 0;
        }
        k_memcpy(out, s_ramdisk + offset, EXT2_BLOCK_SIZE);
        return 1;
    }

    return block_read(s_block_dev,
                      abs_lba_for_block(block),
                      EXT2_SECTORS_PER_BLOCK,
                      out);
}

static int write_block(u32 block, const u8* data) {
    if (s_ramdisk) {
        u32 offset = block * EXT2_BLOCK_SIZE;
        if (offset > s_ramdisk_size || s_ramdisk_size - offset < EXT2_BLOCK_SIZE) {
            return 0;
        }
        k_memcpy(s_ramdisk + offset, data, EXT2_BLOCK_SIZE);
        return 1;
    }

    return block_write(s_block_dev,
                       abs_lba_for_block(block),
                       EXT2_SECTORS_PER_BLOCK,
                       data);
}

static int write_blocks(u32 first_block, u32 block_count, const u8* data) {
    u32 sectors = block_count * EXT2_SECTORS_PER_BLOCK;

    if (block_count == 0u) return 1;
    if (s_ramdisk) {
        u32 offset = first_block * EXT2_BLOCK_SIZE;
        u32 bytes = block_count * EXT2_BLOCK_SIZE;
        if (offset > s_ramdisk_size || s_ramdisk_size - offset < bytes) {
            return 0;
        }
        k_memcpy(s_ramdisk + offset, data, bytes);
        return 1;
    }
    if (sectors > 255u) return 0;
    return block_write(s_block_dev,
                       abs_lba_for_block(first_block),
                       sectors,
                       data);
}

static int read_bitmaps(void) {
    if (s_bitmaps_loaded) return 1;

    return read_block(EXT2_BLOCK_BITMAP_BLOCK, s_block_bitmap) &&
           read_block(EXT2_INODE_BITMAP_BLOCK, s_inode_bitmap) &&
           (s_bitmaps_loaded = 1);
}

static int write_block_bitmap(void) {
    if (s_bitmap_write_defer_depth != 0u) {
        s_block_bitmap_dirty = 1;
        return 1;
    }
    if (!write_block(EXT2_BLOCK_BITMAP_BLOCK, s_block_bitmap)) return 0;
    s_block_bitmap_dirty = 0;
    return 1;
}

static int write_inode_bitmap(void) {
    if (s_bitmap_write_defer_depth != 0u) {
        s_inode_bitmap_dirty = 1;
        return 1;
    }
    if (!write_block(EXT2_INODE_BITMAP_BLOCK, s_inode_bitmap)) return 0;
    s_inode_bitmap_dirty = 0;
    return 1;
}

static void bitmap_write_defer_begin(void) {
    s_bitmap_write_defer_depth++;
}

static int bitmap_write_defer_end(void) {
    if (s_bitmap_write_defer_depth == 0u) return 1;

    s_bitmap_write_defer_depth--;
    if (s_bitmap_write_defer_depth != 0u) return 1;

    if (s_block_bitmap_dirty &&
        !write_block(EXT2_BLOCK_BITMAP_BLOCK, s_block_bitmap)) {
        return 0;
    }
    s_block_bitmap_dirty = 0;

    if (s_inode_bitmap_dirty &&
        !write_block(EXT2_INODE_BITMAP_BLOCK, s_inode_bitmap)) {
        return 0;
    }
    s_inode_bitmap_dirty = 0;
    return 1;
}

static int read_inode(u32 ino, ext2_inode_t* out) {
    if (!out || ino == 0 || ino > EXT2_TOTAL_INODES) return 0;

    u32 index = ino - 1u;
    u32 byte_off = index * EXT2_INODE_SIZE;
    u32 block = EXT2_INODE_TABLE_BLOCK + byte_off / EXT2_BLOCK_SIZE;
    u32 off = byte_off % EXT2_BLOCK_SIZE;

    if (!read_block(block, s_block)) return 0;
    out->mode = read_u16_le(s_block, off + INODE_MODE);
    out->uid = read_u16_le(s_block, off + INODE_UID);
    out->size = read_u32_le(s_block, off + INODE_SIZE);
    out->atime = read_u32_le(s_block, off + INODE_ATIME);
    out->ctime = read_u32_le(s_block, off + INODE_CTIME);
    out->mtime = read_u32_le(s_block, off + INODE_MTIME);
    out->gid = read_u16_le(s_block, off + INODE_GID);
    out->links_count = read_u16_le(s_block, off + INODE_LINKS);
    out->blocks_512 = read_u32_le(s_block, off + INODE_BLOCKS);
    for (u32 i = 0; i < EXT2_N_BLOCKS; i++) {
        out->block[i] = read_u32_le(s_block, off + INODE_BLOCK + i * 4u);
    }
    return 1;
}

static int write_inode(u32 ino, const ext2_inode_t* in) {
    if (!in || ino == 0 || ino > EXT2_TOTAL_INODES) return 0;

    u32 index = ino - 1u;
    u32 byte_off = index * EXT2_INODE_SIZE;
    u32 block = EXT2_INODE_TABLE_BLOCK + byte_off / EXT2_BLOCK_SIZE;
    u32 off = byte_off % EXT2_BLOCK_SIZE;

    if (!read_block(block, s_block)) return 0;
    write_u16_le(s_block, off + INODE_MODE, in->mode);
    write_u16_le(s_block, off + INODE_UID, in->uid);
    write_u32_le(s_block, off + INODE_SIZE, in->size);
    write_u32_le(s_block, off + INODE_ATIME, in->atime);
    write_u32_le(s_block, off + INODE_CTIME, in->ctime);
    write_u32_le(s_block, off + INODE_MTIME, in->mtime);
    write_u32_le(s_block, off + INODE_DTIME, 0);
    write_u16_le(s_block, off + INODE_GID, in->gid);
    write_u16_le(s_block, off + INODE_LINKS, in->links_count);
    write_u32_le(s_block, off + INODE_BLOCKS, in->blocks_512);
    write_u32_le(s_block, off + INODE_FLAGS, 0);
    for (u32 i = 0; i < EXT2_N_BLOCKS; i++) {
        write_u32_le(s_block, off + INODE_BLOCK + i * 4u, in->block[i]);
    }
    return write_block(block, s_block);
}

static int inode_is_dir(const ext2_inode_t* inode) {
    return inode && ((inode->mode & 0xF000u) == EXT2_S_IFDIR);
}

static int inode_is_file(const ext2_inode_t* inode) {
    return inode && ((inode->mode & 0xF000u) == EXT2_S_IFREG);
}

static int alloc_block_with_zero(u32* out_block, int zero_block) {
    if (!out_block || !read_bitmaps()) return 0;

    if (s_next_alloc_block < EXT2_FIRST_DATA_BLOCK ||
        s_next_alloc_block >= EXT2_TOTAL_BLOCKS) {
        s_next_alloc_block = EXT2_FIRST_DATA_BLOCK;
    }

    for (u32 pass = 0; pass < 2u; pass++) {
        u32 begin = pass == 0u ? s_next_alloc_block : EXT2_FIRST_DATA_BLOCK;
        u32 end = pass == 0u ? EXT2_TOTAL_BLOCKS : s_next_alloc_block;

        for (u32 block = begin; block < end; block++) {
            if (bit_test(s_block_bitmap, block)) continue;

            bit_set(s_block_bitmap, block);
            if (!write_block_bitmap()) return 0;
            if (zero_block) {
                k_memset(s_block, 0, EXT2_BLOCK_SIZE);
                if (!write_block(block, s_block)) return 0;
            }
            s_next_alloc_block = block + 1u;
            if (s_next_alloc_block >= EXT2_TOTAL_BLOCKS) {
                s_next_alloc_block = EXT2_FIRST_DATA_BLOCK;
            }
            *out_block = block;
            return 1;
        }
    }
    return 0;
}

static int alloc_block(u32* out_block) {
    return alloc_block_with_zero(out_block, 1);
}

static int alloc_block_uninit(u32* out_block) {
    return alloc_block_with_zero(out_block, 0);
}

static int alloc_inode(u32* out_ino) {
    if (!out_ino || !read_bitmaps()) return 0;

    for (u32 ino = EXT2_FIRST_NONRES_INO; ino <= EXT2_TOTAL_INODES; ino++) {
        if (!bit_test(s_inode_bitmap, ino - 1u)) {
            bit_set(s_inode_bitmap, ino - 1u);
            if (!write_inode_bitmap()) return 0;
            *out_ino = ino;
            return 1;
        }
    }
    return 0;
}

static int free_block(u32 block) {
    if (block == 0 || block >= EXT2_TOTAL_BLOCKS) return 1;
    if (!read_bitmaps()) return 0;
    bit_clear(s_block_bitmap, block);
    if (block < s_next_alloc_block) s_next_alloc_block = block;
    return write_block_bitmap();
}

static int free_inode(u32 ino) {
    if (ino == 0 || ino > EXT2_TOTAL_INODES) return 1;
    if (!read_bitmaps()) return 0;
    bit_clear(s_inode_bitmap, ino - 1u);
    return write_inode_bitmap();
}

static int alloc_block_run(u32 count, u32* out_first) {
    u32 run_first = 0;
    u32 run_count = 0;

    if (!out_first || count == 0u || !read_bitmaps()) return 0;
    if (s_next_alloc_block < EXT2_FIRST_DATA_BLOCK ||
        s_next_alloc_block >= EXT2_TOTAL_BLOCKS) {
        s_next_alloc_block = EXT2_FIRST_DATA_BLOCK;
    }

    for (u32 pass = 0; pass < 2u; pass++) {
        u32 begin = pass == 0u ? s_next_alloc_block : EXT2_FIRST_DATA_BLOCK;
        u32 end = pass == 0u ? EXT2_TOTAL_BLOCKS : s_next_alloc_block;

        run_first = 0;
        run_count = 0;
        for (u32 block = begin; block < end; block++) {
            if (!bit_test(s_block_bitmap, block)) {
                if (run_count == 0u) run_first = block;
                run_count++;
                if (run_count == count) {
                    for (u32 i = 0; i < count; i++) {
                        bit_set(s_block_bitmap, run_first + i);
                    }
                    if (!write_block_bitmap()) return 0;
                    s_next_alloc_block = run_first + count;
                    if (s_next_alloc_block >= EXT2_TOTAL_BLOCKS) {
                        s_next_alloc_block = EXT2_FIRST_DATA_BLOCK;
                    }
                    *out_first = run_first;
                    return 1;
                }
            } else {
                run_count = 0;
            }
        }
    }
    return 0;
}

static u32 ptr_block_entry(u32 block, u32 index) {
    if (!block || index >= EXT2_PTRS_PER_BLOCK) return 0;
    if (!read_block(block, s_block2)) return 0;
    return read_u32_le(s_block2, index * 4u);
}

static int set_ptr_block_entry(u32 block, u32 index, u32 value) {
    if (!block || index >= EXT2_PTRS_PER_BLOCK) return 0;
    if (!read_block(block, s_block2)) return 0;
    write_u32_le(s_block2, index * 4u, value);
    return write_block(block, s_block2);
}

static u32 inode_block_count(const ext2_inode_t* inode) {
    if (!inode) return 0;
    return (inode->size + EXT2_BLOCK_SIZE - 1u) / EXT2_BLOCK_SIZE;
}

static int inode_get_data_block_ex(ext2_inode_t* inode,
                                   u32 logical,
                                   int create,
                                   int zero_new_data,
                                   u32* out_block) {
    u32 block = 0;

    if (!inode || !out_block) return 0;

    if (logical < EXT2_DIRECT_BLOCKS) {
        if (inode->block[logical] == 0 && create) {
            if (zero_new_data) {
                if (!alloc_block(&inode->block[logical])) return 0;
            } else if (!alloc_block_uninit(&inode->block[logical])) {
                return 0;
            }
            inode->blocks_512 += EXT2_SECTORS_PER_BLOCK;
        }
        *out_block = inode->block[logical];
        return 1;
    }

    logical -= EXT2_DIRECT_BLOCKS;
    if (logical < EXT2_PTRS_PER_BLOCK) {
        if (inode->block[12] == 0 && create) {
            if (!alloc_block(&inode->block[12])) return 0;
            inode->blocks_512 += EXT2_SECTORS_PER_BLOCK;
        }
        if (inode->block[12] == 0) {
            *out_block = 0;
            return 1;
        }
        block = ptr_block_entry(inode->block[12], logical);
        if (block == 0 && create) {
            if (zero_new_data) {
                if (!alloc_block(&block)) return 0;
            } else if (!alloc_block_uninit(&block)) {
                return 0;
            }
            if (!set_ptr_block_entry(inode->block[12], logical, block)) return 0;
            inode->blocks_512 += EXT2_SECTORS_PER_BLOCK;
        }
        *out_block = block;
        return 1;
    }

    logical -= EXT2_PTRS_PER_BLOCK;
    u32 outer = logical / EXT2_PTRS_PER_BLOCK;
    u32 inner = logical % EXT2_PTRS_PER_BLOCK;
    if (outer >= EXT2_PTRS_PER_BLOCK) return 0;

    if (inode->block[13] == 0 && create) {
        if (!alloc_block(&inode->block[13])) return 0;
        inode->blocks_512 += EXT2_SECTORS_PER_BLOCK;
    }
    if (inode->block[13] == 0) {
        *out_block = 0;
        return 1;
    }

    u32 indirect = ptr_block_entry(inode->block[13], outer);
    if (indirect == 0 && create) {
        if (!alloc_block(&indirect)) return 0;
        if (!set_ptr_block_entry(inode->block[13], outer, indirect)) return 0;
        inode->blocks_512 += EXT2_SECTORS_PER_BLOCK;
    }
    if (indirect == 0) {
        *out_block = 0;
        return 1;
    }

    block = ptr_block_entry(indirect, inner);
    if (block == 0 && create) {
        if (zero_new_data) {
            if (!alloc_block(&block)) return 0;
        } else if (!alloc_block_uninit(&block)) {
            return 0;
        }
        if (!set_ptr_block_entry(indirect, inner, block)) return 0;
        inode->blocks_512 += EXT2_SECTORS_PER_BLOCK;
    }
    *out_block = block;
    return 1;
}

static int inode_get_data_block(ext2_inode_t* inode,
                                u32 logical,
                                int create,
                                u32* out_block) {
    return inode_get_data_block_ex(inode, logical, create, 1, out_block);
}

static int inode_prepare_fresh_blocks(ext2_inode_t* inode,
                                      u32 logical,
                                      u32 max_blocks,
                                      u32* out_first_block,
                                      u32* out_blocks) {
    u32 blocks;
    u32 first = 0;

    if (!inode || !out_first_block || !out_blocks || max_blocks == 0u) return -1;

    if (logical < EXT2_DIRECT_BLOCKS) {
        blocks = EXT2_DIRECT_BLOCKS - logical;
        if (blocks > max_blocks) blocks = max_blocks;
        for (u32 i = 0; i < blocks; i++) {
            if (inode->block[logical + i] != 0u) return 0;
        }
        if (!alloc_block_run(blocks, &first)) return -1;
        for (u32 i = 0; i < blocks; i++) {
            inode->block[logical + i] = first + i;
        }
        inode->blocks_512 += blocks * EXT2_SECTORS_PER_BLOCK;
        *out_first_block = first;
        *out_blocks = blocks;
        return 1;
    }

    logical -= EXT2_DIRECT_BLOCKS;
    if (logical < EXT2_PTRS_PER_BLOCK) {
        blocks = EXT2_PTRS_PER_BLOCK - logical;
        if (blocks > max_blocks) blocks = max_blocks;

        if (inode->block[12] == 0u) {
            if (!alloc_block_uninit(&inode->block[12])) return -1;
            inode->blocks_512 += EXT2_SECTORS_PER_BLOCK;
            k_memset(s_block2, 0, EXT2_BLOCK_SIZE);
        } else if (!read_block(inode->block[12], s_block2)) {
            return -1;
        }

        for (u32 i = 0; i < blocks; i++) {
            if (read_u32_le(s_block2, (logical + i) * 4u) != 0u) return 0;
        }
        if (!alloc_block_run(blocks, &first)) return -1;
        for (u32 i = 0; i < blocks; i++) {
            write_u32_le(s_block2, (logical + i) * 4u, first + i);
        }
        if (!write_block(inode->block[12], s_block2)) return -1;
        inode->blocks_512 += blocks * EXT2_SECTORS_PER_BLOCK;
        *out_first_block = first;
        *out_blocks = blocks;
        return 1;
    }

    logical -= EXT2_PTRS_PER_BLOCK;
    u32 outer = logical / EXT2_PTRS_PER_BLOCK;
    u32 inner = logical % EXT2_PTRS_PER_BLOCK;
    if (outer >= EXT2_PTRS_PER_BLOCK) return -1;

    blocks = EXT2_PTRS_PER_BLOCK - inner;
    if (blocks > max_blocks) blocks = max_blocks;

    if (inode->block[13] == 0u) {
        if (!alloc_block_uninit(&inode->block[13])) return -1;
        inode->blocks_512 += EXT2_SECTORS_PER_BLOCK;
        k_memset(s_block, 0, EXT2_BLOCK_SIZE);
    } else if (!read_block(inode->block[13], s_block)) {
        return -1;
    }

    u32 indirect = read_u32_le(s_block, outer * 4u);
    if (indirect == 0u) {
        if (!alloc_block_uninit(&indirect)) return -1;
        inode->blocks_512 += EXT2_SECTORS_PER_BLOCK;
        write_u32_le(s_block, outer * 4u, indirect);
        if (!write_block(inode->block[13], s_block)) return -1;
        k_memset(s_block2, 0, EXT2_BLOCK_SIZE);
    } else if (!read_block(indirect, s_block2)) {
        return -1;
    }

    for (u32 i = 0; i < blocks; i++) {
        if (read_u32_le(s_block2, (inner + i) * 4u) != 0u) return 0;
    }
    if (!alloc_block_run(blocks, &first)) return -1;
    for (u32 i = 0; i < blocks; i++) {
        write_u32_le(s_block2, (inner + i) * 4u, first + i);
    }
    if (!write_block(indirect, s_block2)) return -1;
    inode->blocks_512 += blocks * EXT2_SECTORS_PER_BLOCK;
    *out_first_block = first;
    *out_blocks = blocks;
    return 1;
}

static int free_inode_blocks(ext2_inode_t* inode) {
    if (!inode) return 0;

    for (u32 i = 0; i < EXT2_DIRECT_BLOCKS; i++) {
        if (inode->block[i]) {
            if (!free_block(inode->block[i])) return 0;
            inode->block[i] = 0;
        }
    }

    if (inode->block[12]) {
        if (!read_block(inode->block[12], s_block)) return 0;
        for (u32 i = 0; i < EXT2_PTRS_PER_BLOCK; i++) {
            u32 block = read_u32_le(s_block, i * 4u);
            if (block && !free_block(block)) return 0;
        }
        if (!free_block(inode->block[12])) return 0;
        inode->block[12] = 0;
    }

    if (inode->block[13]) {
        if (!read_block(inode->block[13], s_block)) return 0;
        for (u32 i = 0; i < EXT2_PTRS_PER_BLOCK; i++) {
            u32 indirect = read_u32_le(s_block, i * 4u);
            if (!indirect) continue;
            if (!read_block(indirect, s_block2)) return 0;
            for (u32 j = 0; j < EXT2_PTRS_PER_BLOCK; j++) {
                u32 block = read_u32_le(s_block2, j * 4u);
                if (block && !free_block(block)) return 0;
            }
            if (!free_block(indirect)) return 0;
        }
        if (!free_block(inode->block[13])) return 0;
        inode->block[13] = 0;
    }

    inode->block[14] = 0;
    inode->size = 0;
    inode->blocks_512 = 0;
    return 1;
}

static int path_next_component(const char** path,
                               char* out,
                               unsigned int out_size,
                               int* is_last) {
    const char* p = *path;
    while (*p && is_sep(*p)) p++;
    if (*p == '\0') {
        *path = p;
        return 0;
    }

    unsigned int len = 0;
    while (p[len] && !is_sep(p[len])) len++;
    if (len == 0 || len >= out_size) return -1;

    for (unsigned int i = 0; i < len; i++) out[i] = p[i];
    out[len] = '\0';
    p += len;
    while (*p && is_sep(*p)) p++;
    *is_last = (*p == '\0');
    *path = p;
    return 1;
}

static int name_equals(const ext2_dirent_t* de, const char* name) {
    int len = k_strlen(name);
    if (!de || len < 0 || de->name_len != (u8)len) return 0;
    for (int i = 0; i < len; i++) {
        if ((char)de->raw_name[i] != name[i]) return 0;
    }
    return 1;
}

static u16 dir_rec_len(unsigned int name_len) {
    return (u16)((8u + name_len + 3u) & ~3u);
}

static int parse_dirent(const u8* buf, u32 off, ext2_dirent_t* out) {
    if (!buf || !out || off + 8u > EXT2_BLOCK_SIZE) return 0;
    out->inode = read_u32_le(buf, off);
    out->rec_len = read_u16_le(buf, off + 4u);
    out->name_len = buf[off + 6u];
    out->file_type = buf[off + 7u];
    out->raw_name = buf + off + 8u;
    if (out->rec_len < 8u || off + out->rec_len > EXT2_BLOCK_SIZE ||
        out->name_len > out->rec_len - 8u) {
        return 0;
    }
    return 1;
}

static int dir_find_entry(u32 dir_ino,
                          ext2_inode_t* dir,
                          const char* name,
                          u32* out_ino,
                          u8* out_type) {
    if (!dir || !inode_is_dir(dir) || !name) return 0;

    u32 blocks = inode_block_count(dir);
    for (u32 logical = 0; logical < blocks; logical++) {
        u32 block = 0;
        if (!inode_get_data_block(dir, logical, 0, &block)) return 0;
        if (!block) continue;
        if (!read_block(block, s_block)) return 0;

        u32 off = 0;
        while (off < EXT2_BLOCK_SIZE) {
            ext2_dirent_t de;
            if (!parse_dirent(s_block, off, &de)) return 0;
            if (de.inode != 0 && name_equals(&de, name)) {
                if (out_ino) *out_ino = de.inode;
                if (out_type) *out_type = de.file_type;
                return 1;
            }
            off += de.rec_len;
        }
    }

    (void)dir_ino;
    return 0;
}

static int resolve_path(const char* path, resolved_path_t* out) {
    if (!out) return 0;

    out->parent_ino = 0;
    out->ino = EXT2_ROOT_INO;
    out->has_entry = 0;
    if (!read_inode(EXT2_ROOT_INO, &out->inode)) return 0;

    if (!path || path[0] == '\0' || (path[0] == '/' && path[1] == '\0')) {
        out->has_entry = 1;
        return 1;
    }

    const char* cursor = path;
    char component[256];
    u32 cur_ino = EXT2_ROOT_INO;
    ext2_inode_t cur;
    if (!read_inode(cur_ino, &cur)) return 0;

    while (1) {
        int is_last = 0;
        int r = path_next_component(&cursor, component, sizeof(component), &is_last);
        if (r < 0) return 0;
        if (r == 0) break;

        u32 next_ino = 0;
        u8 type = 0;
        if (!dir_find_entry(cur_ino, &cur, component, &next_ino, &type)) {
            return 0;
        }

        ext2_inode_t next;
        if (!read_inode(next_ino, &next)) return 0;

        if (is_last) {
            out->parent_ino = cur_ino;
            out->ino = next_ino;
            out->inode = next;
            out->has_entry = 1;
            return 1;
        }

        if (!inode_is_dir(&next)) return 0;
        cur_ino = next_ino;
        cur = next;
    }

    out->parent_ino = 0;
    out->ino = cur_ino;
    out->inode = cur;
    out->has_entry = 1;
    return 1;
}

static int resolve_create_path(const char* path, create_path_t* out) {
    if (!path || !out) return 0;

    const char* cursor = path;
    char component[256];
    u32 cur_ino = EXT2_ROOT_INO;
    ext2_inode_t cur;
    if (!read_inode(cur_ino, &cur)) return 0;

    int saw = 0;
    while (1) {
        int is_last = 0;
        int r = path_next_component(&cursor, component, sizeof(component), &is_last);
        if (r < 0) return 0;
        if (r == 0) break;
        saw = 1;

        if (is_last) {
            out->parent_ino = cur_ino;
            out->parent = cur;
            k_strncpy(out->leaf, component, sizeof(out->leaf));
            return out->leaf[0] != '\0';
        }

        u32 next_ino = 0;
        if (!dir_find_entry(cur_ino, &cur, component, &next_ino, 0)) return 0;
        ext2_inode_t next;
        if (!read_inode(next_ino, &next) || !inode_is_dir(&next)) return 0;
        cur_ino = next_ino;
        cur = next;
    }

    (void)saw;
    return 0;
}

static int dir_write_entry(u8* buf,
                           u32 off,
                           u32 ino,
                           u16 rec_len,
                           const char* name,
                           u8 type) {
    unsigned int name_len = (unsigned int)k_strlen(name);
    if (name_len > 255u || rec_len < dir_rec_len(name_len) ||
        off + rec_len > EXT2_BLOCK_SIZE) {
        return 0;
    }
    write_u32_le(buf, off, ino);
    write_u16_le(buf, off + 4u, rec_len);
    buf[off + 6u] = (u8)name_len;
    buf[off + 7u] = type;
    for (unsigned int i = 0; i < name_len; i++) {
        buf[off + 8u + i] = (u8)name[i];
    }
    return 1;
}

static int dir_add_entry(u32 dir_ino,
                         ext2_inode_t* dir,
                         const char* name,
                         u32 child_ino,
                         u8 type) {
    if (!dir || !inode_is_dir(dir) || !name || name[0] == '\0') return 0;
    if (dir_find_entry(dir_ino, dir, name, 0, 0)) return 0;

    u16 need = dir_rec_len((unsigned int)k_strlen(name));
    u32 blocks = inode_block_count(dir);
    if (blocks == 0) blocks = 1;

    for (u32 logical = 0; logical < blocks; logical++) {
        u32 block = 0;
        if (!inode_get_data_block(dir, logical, 1, &block)) return 0;
        if (!read_block(block, s_block)) return 0;

        u32 off = 0;
        while (off < EXT2_BLOCK_SIZE) {
            ext2_dirent_t de;
            if (!parse_dirent(s_block, off, &de)) {
                if (off == 0) {
                    if (!dir_write_entry(s_block, 0, child_ino, EXT2_BLOCK_SIZE, name, type)) return 0;
                    if (dir->size < (logical + 1u) * EXT2_BLOCK_SIZE) {
                        dir->size = (logical + 1u) * EXT2_BLOCK_SIZE;
                    }
                    return write_block(block, s_block) && write_inode(dir_ino, dir);
                }
                return 0;
            }

            if (de.inode == 0 && de.rec_len >= need) {
                if (!dir_write_entry(s_block, off, child_ino, de.rec_len, name, type)) return 0;
                return write_block(block, s_block) && write_inode(dir_ino, dir);
            }

            u16 actual = dir_rec_len(de.name_len);
            if (de.inode != 0 && de.rec_len >= actual + need) {
                u16 old_rec = de.rec_len;
                write_u16_le(s_block, off + 4u, actual);
                u32 new_off = off + actual;
                if (!dir_write_entry(s_block, new_off, child_ino,
                                     (u16)(old_rec - actual), name, type)) {
                    return 0;
                }
                return write_block(block, s_block) && write_inode(dir_ino, dir);
            }
            off += de.rec_len;
        }
    }

    if (blocks >= EXT2_DIRECT_BLOCKS) return 0;
    u32 new_block = 0;
    if (!inode_get_data_block(dir, blocks, 1, &new_block)) return 0;
    k_memset(s_block, 0, EXT2_BLOCK_SIZE);
    if (!dir_write_entry(s_block, 0, child_ino, EXT2_BLOCK_SIZE, name, type)) return 0;
    dir->size = (blocks + 1u) * EXT2_BLOCK_SIZE;
    return write_block(new_block, s_block) && write_inode(dir_ino, dir);
}

static int dir_remove_entry(u32 dir_ino, ext2_inode_t* dir, const char* name) {
    if (!dir || !inode_is_dir(dir) || !name) return 0;
    u32 blocks = inode_block_count(dir);

    for (u32 logical = 0; logical < blocks; logical++) {
        u32 block = 0;
        if (!inode_get_data_block(dir, logical, 0, &block)) return 0;
        if (!block) continue;
        if (!read_block(block, s_block)) return 0;

        u32 off = 0;
        while (off < EXT2_BLOCK_SIZE) {
            ext2_dirent_t de;
            if (!parse_dirent(s_block, off, &de)) return 0;
            if (de.inode != 0 && name_equals(&de, name)) {
                write_u32_le(s_block, off, 0);
                return write_block(block, s_block) && write_inode(dir_ino, dir);
            }
            off += de.rec_len;
        }
    }
    return 0;
}

static int dir_is_empty(const ext2_inode_t* dir) {
    ext2_inode_t tmp = *dir;
    u32 blocks = inode_block_count(&tmp);
    for (u32 logical = 0; logical < blocks; logical++) {
        u32 block = 0;
        if (!inode_get_data_block(&tmp, logical, 0, &block)) return 0;
        if (!block) continue;
        if (!read_block(block, s_block)) return 0;
        u32 off = 0;
        while (off < EXT2_BLOCK_SIZE) {
            ext2_dirent_t de;
            if (!parse_dirent(s_block, off, &de)) return 0;
            if (de.inode != 0) {
                char name[3];
                name[0] = (de.name_len > 0) ? (char)de.raw_name[0] : '\0';
                name[1] = (de.name_len > 1) ? (char)de.raw_name[1] : '\0';
                name[2] = '\0';
                if (!(de.name_len == 1 && name[0] == '.') &&
                    !(de.name_len == 2 && name[0] == '.' && name[1] == '.')) {
                    return 0;
                }
            }
            off += de.rec_len;
        }
    }
    return 1;
}

static int mem_source_read(void* ctx, u32 offset, u8* out, u32 len) {
    mem_source_t* mem = (mem_source_t*)ctx;
    if (!mem || !out) return 0;
    k_memcpy(out, mem->data + offset, len);
    return 1;
}

static int mem_sink_write(void* ctx, u32 offset, const u8* data, u32 len) {
    mem_sink_t* mem = (mem_sink_t*)ctx;
    if (!mem || !mem->data || !data) return 0;
    k_memcpy(mem->data + offset, data, len);
    return 1;
}

static int file_read_to_sink(u32 ino,
                             ext2_inode_t* inode,
                             u32 start,
                             u32 len,
                             const ext2_data_sink_t* sink) {
    if (!inode || !sink || !sink->write) return 0;
    if (start > inode->size) return 0;
    if (len > inode->size - start) len = inode->size - start;

    u32 done = 0;
    while (done < len) {
        u32 pos = start + done;
        u32 logical = pos / EXT2_BLOCK_SIZE;
        u32 block_off = pos % EXT2_BLOCK_SIZE;
        u32 chunk = EXT2_BLOCK_SIZE - block_off;
        u32 block = 0;

        if (chunk > len - done) chunk = len - done;
        if (!inode_get_data_block(inode, logical, 0, &block)) return 0;
        if (!block) {
            k_memset(s_block, 0, EXT2_BLOCK_SIZE);
        } else if (!read_block(block, s_block)) {
            return 0;
        }
        if (!sink->write(sink->ctx, done, s_block + block_off, chunk)) return 0;
        done += chunk;
    }

    (void)ino;
    return 1;
}

static int write_file_range(u32 ino,
                            ext2_inode_t* inode,
                            u32 offset,
                            const u8* data,
                            u32 len) {
    u32 done = 0;
    int ok = 1;
    if (!inode || (!data && len > 0)) return 0;

    bitmap_write_defer_begin();
    while (done < len) {
        u32 pos = offset + done;
        u32 logical = pos / EXT2_BLOCK_SIZE;
        u32 block_off = pos % EXT2_BLOCK_SIZE;
        u32 chunk = EXT2_BLOCK_SIZE - block_off;
        u32 block = 0;
        if (chunk > len - done) chunk = len - done;

        if (block_off == 0u && len - done >= EXT2_BLOCK_SIZE) {
            u32 max_blocks = (len - done) / EXT2_BLOCK_SIZE;
            u32 blocks = 1;
            int prepared;

            if (max_blocks > EXT2_WRITE_CHUNK_SIZE / EXT2_BLOCK_SIZE) {
                max_blocks = EXT2_WRITE_CHUNK_SIZE / EXT2_BLOCK_SIZE;
            }
            prepared = inode_prepare_fresh_blocks(inode,
                                                  logical,
                                                  max_blocks,
                                                  &block,
                                                  &blocks);
            if (prepared < 0) {
                ok = 0;
                break;
            }
            if (prepared > 0) {
                if (!write_blocks(block, blocks, data + done)) {
                    ok = 0;
                    break;
                }
                done += blocks * EXT2_BLOCK_SIZE;
                continue;
            }

            blocks = 1;
            if (!inode_get_data_block_ex(inode, logical, 1, 0, &block) ||
                block == 0u) {
                ok = 0;
                break;
            }
            while (blocks < max_blocks) {
                u32 next_block = 0;

                if (!inode_get_data_block_ex(inode,
                        logical + blocks, 1, 0, &next_block) ||
                    next_block == 0u) {
                    ok = 0;
                    break;
                }
                if (next_block != block + blocks) {
                    break;
                }
                blocks++;
            }
            if (!ok) break;
            if (!write_blocks(block, blocks, data + done)) {
                ok = 0;
                break;
            }
            done += blocks * EXT2_BLOCK_SIZE;
            continue;
        }

        if (!inode_get_data_block(inode, logical, 1, &block)) {
            ok = 0;
            break;
        }
        if (chunk != EXT2_BLOCK_SIZE) {
            if (!read_block(block, s_block)) {
                ok = 0;
                break;
            }
        } else {
            k_memset(s_block, 0, EXT2_BLOCK_SIZE);
        }
        k_memcpy(s_block + block_off, data + done, chunk);
        if (!write_block(block, s_block)) {
            ok = 0;
            break;
        }
        done += chunk;
    }
    if (!bitmap_write_defer_end()) ok = 0;
    if (!ok) return 0;

    if (offset + len > inode->size) inode->size = offset + len;
    return write_inode(ino, inode);
}

static int truncate_file(u32 ino, ext2_inode_t* inode) {
    if (!inode) return 0;
    bitmap_write_defer_begin();
    if (!free_inode_blocks(inode)) {
        (void)bitmap_write_defer_end();
        return 0;
    }
    if (!bitmap_write_defer_end()) return 0;
    inode->size = 0;
    inode->blocks_512 = 0;
    return write_inode(ino, inode);
}

static int create_file_in_parent(create_path_t* create,
                                 u32* out_ino,
                                 ext2_inode_t* out_inode) {
    u32 existing = 0;
    if (!create || !out_ino || !out_inode) return 0;
    if (dir_find_entry(create->parent_ino, &create->parent, create->leaf, &existing, 0)) {
        if (!read_inode(existing, out_inode) || !inode_is_file(out_inode)) return 0;
        *out_ino = existing;
        return 1;
    }

    u32 ino = 0;
    if (!alloc_inode(&ino)) return 0;
    ext2_inode_t inode;
    k_memset(&inode, 0, sizeof(inode));
    inode.mode = EXT2_S_IFREG | 0644u;
    inode.links_count = 1;
    inode.size = 0;
    inode.blocks_512 = 0;
    if (!write_inode(ino, &inode)) return 0;
    if (!dir_add_entry(create->parent_ino, &create->parent, create->leaf,
                       ino, EXT2_FT_REG_FILE)) {
        free_inode(ino);
        return 0;
    }
    *out_ino = ino;
    *out_inode = inode;
    return 1;
}

static int write_file_from_source(u32 ino,
                                  ext2_inode_t* inode,
                                  const ext2_data_source_t* source,
                                  u32 size) {
    if (!inode) return 0;
    if (size > EXT2_MAX_WRITE_FILE_BYTES) return 0;
    if (!truncate_file(ino, inode)) return 0;
    if (size == 0) return 1;
    if (!source || !source->read) return 0;

    u32 offset = 0;
    while (offset < size) {
        u32 chunk = size - offset;
        if (chunk > EXT2_WRITE_CHUNK_SIZE) chunk = EXT2_WRITE_CHUNK_SIZE;
        if (!source->read(source->ctx, offset, s_data_buf, chunk)) return 0;
        if (!write_file_range(ino, inode, offset, s_data_buf, chunk)) return 0;
        offset += chunk;
    }
    inode->size = size;
    return write_inode(ino, inode);
}

static unsigned int decimal_len(u32 value) {
    unsigned int len = 1;
    while (value >= 10u) {
        value /= 10u;
        len++;
    }
    return len;
}

static void print_right_aligned_uint(u32 value, unsigned int width) {
    unsigned int len = decimal_len(value);
    while (len < width) {
        terminal_putc(' ');
        len++;
    }
    terminal_put_uint(value);
}

static void print_dir_name(const ext2_dirent_t* de, int is_dir, unsigned int width) {
    unsigned int printed = 0;
    for (u32 i = 0; i < de->name_len; i++) {
        terminal_putc((char)de->raw_name[i]);
        printed++;
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

static int wildcard_match(const char* pattern, const char* text) {
    if (*pattern == '*') {
        while (*pattern == '*') pattern++;
        if (*pattern == '\0') return 1;
        while (*text) {
            if (wildcard_match(pattern, text)) return 1;
            text++;
        }
        return 0;
    }
    if (*pattern == '?') {
        if (*text == '\0') return 0;
        return wildcard_match(pattern + 1, text + 1);
    }
    if (*pattern == '\0') return *text == '\0';
    if (*text == '\0') return 0;
    if (*pattern != *text) return 0;
    return wildcard_match(pattern + 1, text + 1);
}

static int dirent_name_to_buf(const ext2_dirent_t* de, char* out, u32 out_size, int slash) {
    if (!de || !out || out_size == 0) return 0;
    u32 need = de->name_len + (slash ? 1u : 0u) + 1u;
    if (need > out_size) return 0;
    for (u32 i = 0; i < de->name_len; i++) out[i] = (char)de->raw_name[i];
    if (slash) out[de->name_len] = '/';
    out[need - 1u] = '\0';
    return 1;
}

int ext2_is_read_only(void) {
    return !s_ramdisk && s_block_dev && s_block_dev->read_only;
}

block_device_t* ext2_block_device(void) {
    return s_ramdisk ? 0 : s_block_dev;
}

static int ext2_read_only(void) {
    return ext2_is_read_only();
}

void ext2_use_boot_ramdisk(int enable) {
    s_use_boot_ramdisk = enable ? 1 : 0;
}

void ext2_use_block_device(block_device_t* dev) {
    s_block_dev = dev;
    s_use_boot_ramdisk = 0;
}

int ext2_init(void) {
    s_initialised = 0;
    s_bitmaps_loaded = 0;
    s_block_bitmap_dirty = 0;
    s_inode_bitmap_dirty = 0;
    s_bitmap_write_defer_depth = 0;
    s_next_alloc_block = EXT2_FIRST_DATA_BLOCK;

    if (!s_load_buf) {
        s_load_buf = (u8*)kmalloc(EXT2_MAX_LOAD_FILE_BYTES);
        if (!s_load_buf) {
            terminal_puts("ext2: cannot allocate load buffer\n");
            return 0;
        }
    }

    if (s_use_boot_ramdisk && boot_info_ramdisk_valid()) {
        s_ramdisk = (u8*)paging_phys_to_kernel_virt(boot_info_ramdisk_phys());
        s_ramdisk_size = boot_info_ramdisk_size();
        s_ext2_lba = 0;
        s_ext2_sectors = s_ramdisk_size / SECTOR_SIZE;
    } else {
        s_ramdisk = 0;
        s_ramdisk_size = 0;
        if (!s_block_dev) {
            terminal_puts("ext2: no block device selected\n");
            return 0;
        }
        if (s_block_dev->sector_size != SECTOR_SIZE) {
            terminal_puts("ext2: unsupported block sector size\n");
            return 0;
        }
        if (!ext2_alloc_scratch_buffers()) {
            terminal_puts("ext2: cannot allocate scratch buffers\n");
            return 0;
        }
        if (!block_read(s_block_dev, 0, 1, s_sector)) {
            terminal_puts("ext2: cannot read sector 0\n");
            return 0;
        }
        if (s_sector[510] != 0x55 || s_sector[511] != 0xAA) {
            terminal_puts("ext2: bad MBR signature\n");
            return 0;
        }

        u32 entry_off = MBR_PARTITION_TABLE_OFFSET +
                        EXT2_PARTITION_ENTRY_INDEX * MBR_PARTITION_ENTRY_SIZE;
        if (s_sector[entry_off + MBR_PARTITION_TYPE_OFFSET] != EXT2_PARTITION_TYPE) {
            terminal_puts("ext2: MBR partition type mismatch\n");
            return 0;
        }
        s_ext2_lba = read_u32_le(s_sector, entry_off + MBR_PARTITION_LBA_OFFSET);
        s_ext2_sectors = read_u32_le(s_sector, entry_off + MBR_PARTITION_SIZE_OFFSET);
        if (s_ext2_lba == 0 || s_ext2_sectors == 0) {
            terminal_puts("ext2: partition entry not populated\n");
            return 0;
        }
    }

    if (!ext2_alloc_scratch_buffers()) {
        terminal_puts("ext2: cannot allocate scratch buffers\n");
        return 0;
    }

    if (!read_block(0, s_block)) {
        terminal_puts("ext2: cannot read superblock\n");
        return 0;
    }
    u32 sb = 1024u;
    if (read_u16_le(s_block, sb + 56u) != EXT2_SUPER_MAGIC) {
        terminal_puts("ext2: bad superblock magic\n");
        return 0;
    }
    if (read_u32_le(s_block, sb + 24u) != 2u ||
        read_u32_le(s_block, sb + 0u) != EXT2_TOTAL_INODES ||
        read_u32_le(s_block, sb + 4u) != EXT2_TOTAL_BLOCKS ||
        read_u16_le(s_block, sb + 88u) != EXT2_INODE_SIZE) {
        terminal_puts("ext2: unsupported geometry\n");
        return 0;
    }
    if (!read_inode(EXT2_ROOT_INO, &(ext2_inode_t){0})) {
        terminal_puts("ext2: root inode read failed\n");
        return 0;
    }

    s_initialised = 1;
    terminal_puts("ext2: ok  ");
    if (s_ramdisk) {
        terminal_puts("ramdisk=");
        terminal_put_uint(s_ramdisk_size / 1024u);
        terminal_puts(" KB");
    } else {
        terminal_puts("lba=");
        terminal_put_uint(s_ext2_lba);
        terminal_puts(" dev=");
        terminal_puts(s_block_dev && s_block_dev->name ? s_block_dev->name : "?");
    }
    terminal_putc('\n');
    return 1;
}

int ext2_fsinfo(ext2_fsinfo_t* out) {
    if (!out || !s_initialised || !read_bitmaps()) return 0;

    u32 free_blocks = 0;
    for (u32 i = EXT2_FIRST_DATA_BLOCK; i < EXT2_TOTAL_BLOCKS; i++) {
        if (!bit_test(s_block_bitmap, i)) free_blocks++;
    }
    u32 data_blocks = EXT2_TOTAL_BLOCKS - EXT2_FIRST_DATA_BLOCK;
    out->cluster_bytes = EXT2_BLOCK_SIZE;
    out->total_clusters = data_blocks;
    out->free_clusters = free_blocks;
    out->total_bytes = data_blocks * EXT2_BLOCK_SIZE;
    out->free_bytes = free_blocks * EXT2_BLOCK_SIZE;
    out->used_bytes = out->total_bytes - out->free_bytes;
    return 1;
}

int ext2_fsmap(u32 start_cluster, u32 max_clusters, u8* states, u32* out_clusters) {
    if (!states || !out_clusters || !s_initialised || !read_bitmaps()) return 0;
    u32 data_blocks = EXT2_TOTAL_BLOCKS - EXT2_FIRST_DATA_BLOCK;
    if (start_cluster >= data_blocks || max_clusters == 0) {
        *out_clusters = 0;
        return 1;
    }
    u32 count = data_blocks - start_cluster;
    if (count > max_clusters) count = max_clusters;
    for (u32 i = 0; i < count; i++) {
        u32 block = EXT2_FIRST_DATA_BLOCK + start_cluster + i;
        states[i] = bit_test(s_block_bitmap, block) ? 1u : 0u;
    }
    *out_clusters = count;
    return 1;
}

void ext2_ls_path_filtered(const char* path, const char* pattern) {
    if (!s_initialised) {
        terminal_puts("ext2: not initialised\n");
        return;
    }

    resolved_path_t resolved;
    if (!resolve_path(path, &resolved) || !inode_is_dir(&resolved.inode)) {
        terminal_puts("ext2: not found: ");
        terminal_puts(path ? path : "");
        terminal_putc('\n');
        return;
    }

    terminal_puts(path && path[0] ? "ext2 directory: " : "ext2 root directory");
    if (path && path[0]) terminal_puts(path);
    terminal_putc('\n');
    terminal_puts("  name  size  inode\n");

    u32 blocks = inode_block_count(&resolved.inode);
    for (u32 logical = 0; logical < blocks; logical++) {
        u32 block = 0;
        if (!inode_get_data_block(&resolved.inode, logical, 0, &block) || !block) continue;
        if (!read_block(block, s_block)) return;
        u32 off = 0;
        while (off < EXT2_BLOCK_SIZE) {
            ext2_dirent_t de;
            if (!parse_dirent(s_block, off, &de)) return;
            if (de.inode != 0) {
                char name[256];
                int is_dir = de.file_type == EXT2_FT_DIR;
                if (dirent_name_to_buf(&de, name, sizeof(name), 0) &&
                    !(k_strcmp(name, ".") || k_strcmp(name, "..")) &&
                    (!pattern || wildcard_match(pattern, name))) {
                    ext2_inode_t child;
                    if (read_inode(de.inode, &child)) {
                        terminal_puts("  ");
                        terminal_puts(name);
                        if (is_dir) terminal_putc('/');
                        for (unsigned int pad = (unsigned int)k_strlen(name) + (is_dir ? 1u : 0u);
                             pad < 16u; pad++) {
                            terminal_putc(' ');
                        }
                        terminal_puts("  ");
                        print_right_aligned_uint(is_dir ? 0 : child.size, 8);
                        terminal_puts(is_dir ? "-" : "B");
                        terminal_puts("  ");
                        terminal_put_uint(de.inode);
                        terminal_putc('\n');
                    }
                }
            }
            off += de.rec_len;
        }
    }
}

void ext2_ls_path(const char* path) {
    ext2_ls_path_filtered(path, 0);
}

void ext2_ls(void) {
    ext2_ls_path(0);
}

int ext2_stat(const char* path, u32* out_size) {
    resolved_path_t resolved;
    if (!s_initialised || !resolve_path(path, &resolved) ||
        !resolved.has_entry || !inode_is_file(&resolved.inode)) {
        return 0;
    }
    if (out_size) *out_size = resolved.inode.size;
    return 1;
}

int ext2_stat_info(const char* path, ext2_stat_info_t* out) {
    resolved_path_t resolved;

    if (!s_initialised || !out) return 0;
    if (!path || path[0] == '\0') path = "/";
    if (!resolve_path(path, &resolved) || !resolved.has_entry) return 0;
    if (!inode_is_file(&resolved.inode) && !inode_is_dir(&resolved.inode)) return 0;

    k_memset(out, 0, sizeof(*out));
    out->ino = resolved.ino;
    out->mode = resolved.inode.mode;
    out->uid = resolved.inode.uid;
    out->gid = resolved.inode.gid;
    out->links_count = resolved.inode.links_count;
    out->size = resolved.inode.size;
    out->blocks_512 = resolved.inode.blocks_512;
    out->atime = resolved.inode.atime;
    out->ctime = resolved.inode.ctime;
    out->mtime = resolved.inode.mtime;
    out->is_dir = inode_is_dir(&resolved.inode) ? 1 : 0;
    return 1;
}

int ext2_is_dir(const char* path) {
    resolved_path_t resolved;
    if (!s_initialised) return 0;
    if (!path || path[0] == '\0') return 1;
    return resolve_path(path, &resolved) && inode_is_dir(&resolved.inode);
}

const u8* ext2_load(const char* path, u32* out_size) {
    resolved_path_t resolved;
    if (!s_initialised) {
        terminal_puts("ext2: not initialised\n");
        return 0;
    }
    if (!resolve_path(path, &resolved) || !inode_is_file(&resolved.inode)) {
        terminal_puts("ext2: not found: ");
        terminal_puts(path ? path : "");
        terminal_putc('\n');
        return 0;
    }
    if (resolved.inode.size > EXT2_MAX_LOAD_FILE_BYTES) {
        terminal_puts("ext2: file too large\n");
        return 0;
    }
    if (out_size) *out_size = resolved.inode.size;
    if (resolved.inode.size == 0) return s_load_buf;

    mem_sink_t mem = { s_load_buf };
    ext2_data_sink_t sink = { &mem, mem_sink_write };
    if (!file_read_to_sink(resolved.ino, &resolved.inode, 0, resolved.inode.size, &sink)) {
        return 0;
    }
    return s_load_buf;
}

int ext2_read_path_to_sink(const char* path,
                           const ext2_data_sink_t* sink,
                           u32* out_size) {
    resolved_path_t resolved;
    if (!s_initialised || !sink || !sink->write || !out_size) return 0;
    if (!resolve_path(path, &resolved) || !inode_is_file(&resolved.inode)) return 0;
    *out_size = resolved.inode.size;
    return file_read_to_sink(resolved.ino, &resolved.inode, 0, resolved.inode.size, sink);
}

int ext2_read_at_path(const char* path,
                      u32 offset,
                      u8* out,
                      u32 len,
                      u32* out_read) {
    resolved_path_t resolved;
    if (!s_initialised || !out || !out_read) return 0;
    *out_read = 0;
    if (!resolve_path(path, &resolved) || !inode_is_file(&resolved.inode)) return 0;
    if (offset >= resolved.inode.size) return 1;
    if (len > resolved.inode.size - offset) len = resolved.inode.size - offset;

    mem_sink_t mem = { out };
    ext2_data_sink_t sink = { &mem, mem_sink_write };
    if (!file_read_to_sink(resolved.ino, &resolved.inode, offset, len, &sink)) return 0;
    *out_read = len;
    return 1;
}

int ext2_write_path_from_source(const char* path,
                                const ext2_data_source_t* source,
                                u32 size) {
    if (!s_initialised || size > EXT2_MAX_WRITE_FILE_BYTES) return 0;
    if (ext2_read_only()) return 0;
    create_path_t create;
    if (!resolve_create_path(path, &create)) return 0;
    u32 ino = 0;
    ext2_inode_t inode;
    if (!create_file_in_parent(&create, &ino, &inode)) return 0;
    return write_file_from_source(ino, &inode, source, size);
}

int ext2_write_path(const char* path, const u8* data, u32 size) {
    mem_source_t mem = { data ? data : (const u8*)"" };
    ext2_data_source_t source = { &mem, mem_source_read };
    return ext2_write_path_from_source(path, &source, size);
}

int ext2_write(const char* name, const u8* data, u32 size) {
    return ext2_write_path(name, data, size);
}

int ext2_write_at_path(const char* path,
                       u32 offset,
                       const u8* data,
                       u32 len,
                       u32* inout_size,
                       int create) {
    if (!s_initialised || !path || (!data && len > 0) || !inout_size) return 0;
    if (ext2_read_only()) return 0;
    if (offset + len < offset) return 0;
    u32 new_size = offset + len;
    if (new_size < *inout_size) new_size = *inout_size;
    if (new_size > EXT2_MAX_WRITE_FILE_BYTES) return 0;

    resolved_path_t resolved;
    if (!resolve_path(path, &resolved)) {
        if (!create) return 0;
        create_path_t create_path;
        if (!resolve_create_path(path, &create_path)) return 0;
        if (!create_file_in_parent(&create_path, &resolved.ino, &resolved.inode)) return 0;
    }
    if (!inode_is_file(&resolved.inode)) return 0;
    if (len == 0) {
        *inout_size = resolved.inode.size;
        return 1;
    }
    if (!write_file_range(resolved.ino, &resolved.inode, offset, data, len)) return 0;
    *inout_size = resolved.inode.size;
    return 1;
}

int ext2_mkdir(const char* path) {
    if (!s_initialised) return 0;
    if (ext2_read_only()) return 0;
    create_path_t create;
    if (!resolve_create_path(path, &create)) return 0;
    if (dir_find_entry(create.parent_ino, &create.parent, create.leaf, 0, 0)) return 0;

    u32 ino = 0;
    u32 block = 0;
    if (!alloc_inode(&ino) || !alloc_block(&block)) return 0;

    ext2_inode_t inode;
    k_memset(&inode, 0, sizeof(inode));
    inode.mode = EXT2_S_IFDIR | 0755u;
    inode.links_count = 2;
    inode.size = EXT2_BLOCK_SIZE;
    inode.blocks_512 = EXT2_SECTORS_PER_BLOCK;
    inode.block[0] = block;
    if (!write_inode(ino, &inode)) return 0;

    k_memset(s_block, 0, EXT2_BLOCK_SIZE);
    if (!dir_write_entry(s_block, 0, ino, 12, ".", EXT2_FT_DIR)) return 0;
    if (!dir_write_entry(s_block, 12, create.parent_ino,
                         EXT2_BLOCK_SIZE - 12u, "..", EXT2_FT_DIR)) return 0;
    if (!write_block(block, s_block)) return 0;
    if (!dir_add_entry(create.parent_ino, &create.parent, create.leaf, ino, EXT2_FT_DIR)) return 0;
    create.parent.links_count++;
    return write_inode(create.parent_ino, &create.parent);
}

int ext2_rm(const char* path) {
    if (!s_initialised) return 0;
    if (ext2_read_only()) return 0;
    create_path_t create;
    if (!resolve_create_path(path, &create)) return 0;
    u32 ino = 0;
    if (!dir_find_entry(create.parent_ino, &create.parent, create.leaf, &ino, 0)) return 0;
    ext2_inode_t inode;
    if (!read_inode(ino, &inode) || !inode_is_file(&inode)) return 0;
    if (!truncate_file(ino, &inode)) return 0;
    if (!dir_remove_entry(create.parent_ino, &create.parent, create.leaf)) return 0;
    k_memset(&inode, 0, sizeof(inode));
    if (!write_inode(ino, &inode)) return 0;
    return free_inode(ino);
}

int ext2_rmdir(const char* path) {
    if (!s_initialised) return 0;
    if (ext2_read_only()) return 0;
    create_path_t create;
    if (!resolve_create_path(path, &create)) return 0;
    u32 ino = 0;
    if (!dir_find_entry(create.parent_ino, &create.parent, create.leaf, &ino, 0)) return 0;
    if (ino == EXT2_ROOT_INO) return 0;
    ext2_inode_t inode;
    if (!read_inode(ino, &inode) || !inode_is_dir(&inode) || !dir_is_empty(&inode)) return 0;
    if (!free_inode_blocks(&inode)) return 0;
    if (!dir_remove_entry(create.parent_ino, &create.parent, create.leaf)) return 0;
    if (create.parent.links_count > 0) create.parent.links_count--;
    if (!write_inode(create.parent_ino, &create.parent)) return 0;
    k_memset(&inode, 0, sizeof(inode));
    if (!write_inode(ino, &inode)) return 0;
    return free_inode(ino);
}

int ext2_dirent_at(const char* path,
                   u32 index,
                   char* out_name,
                   u32 out_name_size,
                   u32* out_size,
                   int* out_is_dir) {
    resolved_path_t resolved;
    if (!s_initialised || !out_name || out_name_size == 0) return 0;
    if (!resolve_path(path, &resolved) || !inode_is_dir(&resolved.inode)) return 0;

    u32 seen = 0;
    u32 blocks = inode_block_count(&resolved.inode);
    for (u32 logical = 0; logical < blocks; logical++) {
        u32 block = 0;
        if (!inode_get_data_block(&resolved.inode, logical, 0, &block)) return 0;
        if (!block) continue;
        if (!read_block(block, s_block)) return 0;
        u32 off = 0;
        while (off < EXT2_BLOCK_SIZE) {
            ext2_dirent_t de;
            if (!parse_dirent(s_block, off, &de)) return 0;
            if (de.inode != 0) {
                char tmp[256];
                if (dirent_name_to_buf(&de, tmp, sizeof(tmp), 0) &&
                    !(k_strcmp(tmp, ".") || k_strcmp(tmp, ".."))) {
                    if (seen == index) {
                        ext2_inode_t child;
                        if (!read_inode(de.inode, &child)) return 0;
                        int is_dir = inode_is_dir(&child);
                        u32 len = (u32)k_strlen(tmp);
                        u32 need = len + (is_dir ? 1u : 0u) + 1u;
                        if (need > out_name_size) return 0;
                        k_memcpy(out_name, tmp, len);
                        if (is_dir) out_name[len++] = '/';
                        out_name[len] = '\0';
                        if (out_size) *out_size = child.size;
                        if (out_is_dir) *out_is_dir = is_dir ? 1 : 0;
                        return 1;
                    }
                    seen++;
                }
            }
            off += de.rec_len;
        }
    }
    return 0;
}

int ext2_dirents_read(const char* path,
                      u32 start_index,
                      ext2_dirent_info_t* out,
                      u32 max_entries,
                      u32* out_count) {
    resolved_path_t resolved;
    u32 copied = 0;
    u32 seen = 0;

    if (out_count) *out_count = 0;
    if (!s_initialised || !out_count) return 0;
    if (max_entries == 0) return 1;
    if (!out) return 0;
    if (!resolve_path(path, &resolved) || !inode_is_dir(&resolved.inode)) return 0;

    u32 blocks = inode_block_count(&resolved.inode);
    for (u32 logical = 0; logical < blocks; logical++) {
        u32 block = 0;
        if (!inode_get_data_block(&resolved.inode, logical, 0, &block)) return 0;
        if (!block) continue;
        if (!read_block(block, s_block)) return 0;
        u32 off = 0;
        while (off < EXT2_BLOCK_SIZE) {
            ext2_dirent_t de;
            if (!parse_dirent(s_block, off, &de)) return 0;
            if (de.inode != 0) {
                char tmp[256];
                if (dirent_name_to_buf(&de, tmp, sizeof(tmp), 0) &&
                    !(k_strcmp(tmp, ".") || k_strcmp(tmp, ".."))) {
                    if (seen >= start_index) {
                        ext2_inode_t child;
                        if (!read_inode(de.inode, &child)) return 0;
                        int is_dir = inode_is_dir(&child);
                        u32 len = (u32)k_strlen(tmp);
                        u32 need = len + (is_dir ? 1u : 0u) + 1u;
                        if (need > sizeof(out[copied].name)) return 0;

                        k_memset(&out[copied], 0, sizeof(out[copied]));
                        k_memcpy(out[copied].name, tmp, len);
                        if (is_dir) out[copied].name[len++] = '/';
                        out[copied].name[len] = '\0';
                        out[copied].size = child.size;
                        out[copied].is_dir = is_dir ? 1 : 0;
                        copied++;
                        if (copied == max_entries) {
                            *out_count = copied;
                            return 1;
                        }
                        if (!read_block(block, s_block)) return 0;
                    }
                    seen++;
                }
            }
            off += de.rec_len;
        }
    }

    *out_count = copied;
    return 1;
}

int ext2_copy(const char* src, const char* dst) {
    if (ext2_read_only()) return 0;
    u32 size = 0;
    const u8* data = ext2_load(src, &size);
    if (!data) return 0;
    char final_dst[256];
    if (ext2_is_dir(dst)) {
        const char* leaf = src;
        for (const char* p = src; p && *p; p++) {
            if (is_sep(*p)) leaf = p + 1;
        }
        unsigned int pos = 0;
        for (const char* p = dst; *p && pos + 1u < sizeof(final_dst); p++) final_dst[pos++] = *p;
        if (pos > 0 && !is_sep(final_dst[pos - 1u]) && pos + 1u < sizeof(final_dst)) final_dst[pos++] = '/';
        for (const char* p = leaf; *p && pos + 1u < sizeof(final_dst); p++) final_dst[pos++] = *p;
        final_dst[pos] = '\0';
        return ext2_write_path(final_dst, data, size);
    }
    return ext2_write_path(dst, data, size);
}

int ext2_move(const char* src, const char* dst) {
    if (!s_initialised) return 0;
    if (ext2_read_only()) return 0;
    create_path_t src_path;
    if (!resolve_create_path(src, &src_path)) return 0;
    u32 ino = 0;
    u8 type = 0;
    if (!dir_find_entry(src_path.parent_ino, &src_path.parent, src_path.leaf, &ino, &type)) return 0;

    char final_dst[256];
    const char* dst_path = dst;
    if (ext2_is_dir(dst)) {
        unsigned int pos = 0;
        for (const char* p = dst; *p && pos + 1u < sizeof(final_dst); p++) final_dst[pos++] = *p;
        if (pos > 0 && !is_sep(final_dst[pos - 1u]) && pos + 1u < sizeof(final_dst)) final_dst[pos++] = '/';
        for (const char* p = src_path.leaf; *p && pos + 1u < sizeof(final_dst); p++) final_dst[pos++] = *p;
        final_dst[pos] = '\0';
        dst_path = final_dst;
    }

    create_path_t dst_create;
    if (!resolve_create_path(dst_path, &dst_create)) return 0;
    if (dir_find_entry(dst_create.parent_ino, &dst_create.parent, dst_create.leaf, 0, 0)) return 0;
    if (!dir_add_entry(dst_create.parent_ino, &dst_create.parent, dst_create.leaf, ino, type)) return 0;
    if (!dir_remove_entry(src_path.parent_ino, &src_path.parent, src_path.leaf)) return 0;
    return 1;
}
