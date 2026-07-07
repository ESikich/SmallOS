#ifndef USER_LINUX_VERSION_H
#define USER_LINUX_VERSION_H

#define KERNEL_VERSION(a, b, c) (((a) << 16) + ((b) << 8) + (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(5, 10, 0)

#endif /* USER_LINUX_VERSION_H */
