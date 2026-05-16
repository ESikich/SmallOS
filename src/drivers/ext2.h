#ifndef EXT2_H
#define EXT2_H

#include "block.h"

typedef unsigned int   u32;
typedef unsigned short u16;
typedef unsigned char  u8;

#define EXT2_MAX_LOAD_FILE_BYTES   (1024u * 1024u)
#define EXT2_MAX_WRITE_FILE_BYTES  (15u * 1024u * 1024u)

typedef int (*ext2_read_source_fn)(void* ctx, u32 offset, u8* out, u32 len);
typedef int (*ext2_write_sink_fn)(void* ctx, u32 offset, const u8* data, u32 len);

typedef struct {
    void* ctx;
    ext2_read_source_fn read;
} ext2_data_source_t;

typedef struct {
    void* ctx;
    ext2_write_sink_fn write;
} ext2_data_sink_t;

typedef struct {
    u32 total_bytes;
    u32 used_bytes;
    u32 free_bytes;
    u32 cluster_bytes;
    u32 total_clusters;
    u32 free_clusters;
} ext2_fsinfo_t;

typedef struct {
    char name[256];
    u32 size;
    int is_dir;
} ext2_dirent_info_t;

typedef struct {
    u32 ino;
    u16 mode;
    u16 uid;
    u16 gid;
    u16 links_count;
    u32 size;
    u32 blocks_512;
    u32 atime;
    u32 ctime;
    u32 mtime;
    int is_dir;
} ext2_stat_info_t;

int ext2_init(void);
int ext2_is_read_only(void);
void ext2_use_block_device(block_device_t* dev);
void ext2_use_boot_ramdisk(int enable);

void ext2_ls(void);
void ext2_ls_path(const char* path);
void ext2_ls_path_filtered(const char* path, const char* pattern);

int ext2_stat(const char* path, u32* out_size);
int ext2_stat_info(const char* path, ext2_stat_info_t* out);
const u8* ext2_load(const char* path, u32* out_size);
int ext2_read_path_to_sink(const char* path,
                           const ext2_data_sink_t* sink,
                           u32* out_size);
int ext2_read_at_path(const char* path,
                      u32 offset,
                      u8* out,
                      u32 len,
                      u32* out_read);

int ext2_write(const char* name, const u8* data, u32 size);
int ext2_write_path(const char* path, const u8* data, u32 size);
int ext2_write_path_from_source(const char* path,
                                const ext2_data_source_t* source,
                                u32 size);
int ext2_write_at_path(const char* path,
                       u32 offset,
                       const u8* data,
                       u32 len,
                       u32* inout_size,
                       int create);

int ext2_mkdir(const char* path);
int ext2_rmdir(const char* path);
int ext2_rm(const char* path);
int ext2_is_dir(const char* path);
int ext2_dirent_at(const char* path,
                   u32 index,
                   char* out_name,
                   u32 out_name_size,
                   u32* out_size,
                   int* out_is_dir);
int ext2_dirents_read(const char* path,
                      u32 start_index,
                      ext2_dirent_info_t* out,
                      u32 max_entries,
                      u32* out_count);
int ext2_copy(const char* src, const char* dst);
int ext2_move(const char* src, const char* dst);
int ext2_fsinfo(ext2_fsinfo_t* out);
int ext2_fsmap(u32 start_cluster,
               u32 max_clusters,
               u8* states,
               u32* out_clusters);

#endif /* EXT2_H */
