#ifndef USER_SYS_TIMERFD_H
#define USER_SYS_TIMERFD_H

#include "../time.h"
#include "../fcntl.h"

#define TFD_NONBLOCK O_NONBLOCK
#define TFD_CLOEXEC  O_CLOEXEC

struct itimerspec {
    struct timespec it_interval;
    struct timespec it_value;
};

int timerfd_create(int clockid, int flags);
int timerfd_settime(int fd, int flags,
                    const struct itimerspec* new_value,
                    struct itimerspec* old_value);

#endif /* USER_SYS_TIMERFD_H */
