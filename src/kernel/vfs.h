#ifndef VFS_H
#define VFS_H

#include "process.h"

const process_handle_ops_t* vfs_file_ops(void);
void vfs_file_init(fd_entry_t* ent, const char* path, u32 size, int writable);

int vfs_stat(const char* path, u32* out_size, int* out_is_dir);
int vfs_write_root(const char* name, const u8* data, u32 size);
int vfs_write_path(const char* path, const u8* data, u32 size);
int vfs_unlink(const char* path);
int vfs_rename(const char* src, const char* dst);
int vfs_mkdir(const char* path);
int vfs_rmdir(const char* path);
int vfs_dirent_at(const char* path,
                  u32 index,
                  char* out_name,
                  u32 out_name_size,
                  u32* out_size,
                  int* out_is_dir);

#endif /* VFS_H */
