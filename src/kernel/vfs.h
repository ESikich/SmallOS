#ifndef VFS_H
#define VFS_H

#include "process.h"
#include "ext2.h"
#include "uapi_syscall.h"

const process_handle_ops_t* vfs_file_ops(void);
int vfs_file_init(fd_entry_t* ent, const char* path, u32 size, int readable, int writable);
void vfs_file_retain(fd_entry_t* ent);
void vfs_file_set_is_dir(fd_entry_t* ent, int is_dir);
int vfs_file_stat_fd(fd_entry_t* ent, u32* out_size, int* out_is_dir);
int vfs_file_stat_info_fd(fd_entry_t* ent, sys_stat_info_t* out);
int vfs_file_truncate_fd(fd_entry_t* ent, u32 size);
int vfs_file_map_ro_page(fd_entry_t* ent,
                         u32 file_offset,
                         u32* out_frame,
                         u32* out_bytes);
int vfs_file_map_ro_page_path(const char* path,
                              u32 file_offset,
                              u32* out_frame,
                              u32* out_bytes);
int vfs_read_path_at(const char* path, u32 offset, u8* out, u32 len, u32* out_read);
void vfs_file_map_cache_stats(u32* out_pages, u32* out_mapped_refs);

const u8* vfs_load_file(const char* path, u32* out_size);
u8* vfs_load_file_owned(const char* path,
                        u32* out_size,
                        u32* out_frame,
                        u32* out_frames);
void vfs_free_file_owned(u32 frame, u32 frames);
int vfs_stat(const char* path, u32* out_size, int* out_is_dir);
int vfs_stat_info(const char* path, sys_stat_info_t* out);
int vfs_lstat_info(const char* path, sys_stat_info_t* out);
int vfs_is_dir(const char* path);
int vfs_write_root(const char* name, const u8* data, u32 size);
int vfs_write_path(const char* path, const u8* data, u32 size);
int vfs_unlink(const char* path);
int vfs_rename(const char* src, const char* dst);
int vfs_link(const char* oldpath, const char* newpath);
int vfs_symlink(const char* target, const char* linkpath);
int vfs_readlink(const char* path, char* out, u32 out_size, u32* out_len);
int vfs_chmod(const char* path, u16 mode);
int vfs_chown(const char* path, u16 uid, u16 gid);
int vfs_utimes(const char* path, u32 atime, u32 mtime);
int vfs_lutimes(const char* path, u32 atime, u32 mtime);
int vfs_mknod(const char* path, u16 mode, u32 rdev);
int vfs_resize(const char* path, u32 size);
int vfs_mkdir(const char* path);
int vfs_rmdir(const char* path);
int vfs_dirent_at(const char* path,
                  u32 index,
                  char* out_name,
                  u32 out_name_size,
                  u32* out_size,
                  int* out_is_dir);
int vfs_dirents_read(const char* path,
                     u32 start_index,
                     ext2_dirent_info_t* out,
                     u32 max_entries,
                     u32* out_count);

#endif /* VFS_H */
