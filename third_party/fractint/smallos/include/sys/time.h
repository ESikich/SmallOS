#ifndef FRACTINT_SMALLOS_SYS_TIME_H
#define FRACTINT_SMALLOS_SYS_TIME_H

#include_next <sys/time.h>

typedef unsigned int fd_set;

int select(int nfds, fd_set* readfds, fd_set* writefds,
           fd_set* exceptfds, struct timeval* timeout);

#endif
