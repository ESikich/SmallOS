#ifndef USER_SYS_MMAN_WRAPPER_H
#define USER_SYS_MMAN_WRAPPER_H

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED  1
#define MAP_PRIVATE 2
#define MAP_FIXED   0x10
#define MAP_ANON    0x20
#define MAP_ANONYMOUS MAP_ANON

#define MAP_FAILED ((void*)-1)

void* mmap(void* addr, unsigned int length, int prot, int flags, int fd, int offset);
int mprotect(void* addr, unsigned int length, int prot);
int munmap(void* addr, unsigned int length);

#endif
