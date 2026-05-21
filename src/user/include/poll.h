#ifndef USER_POLL_H
#define USER_POLL_H

#include "uapi_poll.h"

int poll(struct pollfd* fds, nfds_t nfds, int timeout);

#endif /* USER_POLL_H */
