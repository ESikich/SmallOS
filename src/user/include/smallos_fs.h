#ifndef SMALLOS_FS_H
#define SMALLOS_FS_H

#include <stdint.h>
#include "uapi_syscall.h"

static inline int smallos_fs_syscall1(int num, uint32_t arg1) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1)
        : "memory"
    );
    return ret;
}

static inline int smallos_fsinfo(sys_fsinfo_t* out_info) {
    return smallos_fs_syscall1(SYS_FSINFO, (uint32_t)out_info);
}

#endif /* SMALLOS_FS_H */
