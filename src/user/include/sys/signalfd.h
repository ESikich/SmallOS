#ifndef USER_SYS_SIGNALFD_H
#define USER_SYS_SIGNALFD_H

#include "../signal.h"
#include "../stdint.h"
#include "../fcntl.h"

#define SFD_NONBLOCK O_NONBLOCK
#define SFD_CLOEXEC  O_CLOEXEC

struct signalfd_siginfo {
    uint32_t ssi_signo;
    uint32_t ssi_errno;
    uint32_t ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    uint32_t ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint8_t  pad[46];
};

int signalfd(int fd, const sigset_t* mask, int flags);

#endif /* USER_SYS_SIGNALFD_H */
