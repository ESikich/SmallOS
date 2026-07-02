#ifndef USER_SYS_SYSMACROS_H
#define USER_SYS_SYSMACROS_H

#define major(dev) ((unsigned int)(((dev) >> 8) & 0xffu))
#define minor(dev) ((unsigned int)((dev) & 0xffu))
#define makedev(maj, min) ((((unsigned int)(maj) & 0xffu) << 8) | ((unsigned int)(min) & 0xffu))

#endif /* USER_SYS_SYSMACROS_H */
