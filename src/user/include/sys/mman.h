#ifndef USER_SYS_MMAN_WRAPPER_H
#define USER_SYS_MMAN_WRAPPER_H

#define PROT_READ  1
#define PROT_WRITE 2
#define PROT_EXEC  4

#define MAP_SHARED  1
#define MAP_PRIVATE 2
#define MAP_FIXED   0x10

#define MAP_FAILED ((void*)-1)

static inline void* mmap(void* addr, unsigned int length, int prot, int flags, int fd, int offset) {
    (void)addr;
    (void)length;
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;
    return MAP_FAILED;
}

static inline int mprotect(void* addr, unsigned int length, int prot) {
    (void)addr;
    (void)length;
    (void)prot;
    return 0;
}

static inline int munmap(void* addr, unsigned int length) {
    (void)addr;
    (void)length;
    return 0;
}

#endif
