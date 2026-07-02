#ifndef USER_SYS_TIME_WRAPPER_H
#define USER_SYS_TIME_WRAPPER_H

#include "../time.h"

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

typedef unsigned int fd_set;

#define FD_SETSIZE 32
#define FD_ZERO(set) (*(set) = 0u)
#define FD_SET(fd, set) do { if ((fd) >= 0 && (fd) < FD_SETSIZE) *(set) |= (1u << (fd)); } while (0)
#define FD_CLR(fd, set) do { if ((fd) >= 0 && (fd) < FD_SETSIZE) *(set) &= ~(1u << (fd)); } while (0)
#define FD_ISSET(fd, set) ((fd) >= 0 && (fd) < FD_SETSIZE && ((*(set) & (1u << (fd))) != 0u))

int gettimeofday(struct timeval* tv, struct timezone* tz);
int settimeofday(const struct timeval* tv, const struct timezone* tz);
int utimes(const char* path, const struct timeval times[2]);
int select(int nfds, fd_set* readfds, fd_set* writefds,
           fd_set* exceptfds, struct timeval* timeout);

#endif
