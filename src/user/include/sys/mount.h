#ifndef USER_SYS_MOUNT_H
#define USER_SYS_MOUNT_H

#define MS_RDONLY 1

#define MNT_FORCE  1
#define MNT_DETACH 2

int mount(const char* source,
          const char* target,
          const char* filesystemtype,
          unsigned long mountflags,
          const void* data);
int umount(const char* target);
int umount2(const char* target, int flags);

#endif /* USER_SYS_MOUNT_H */
