#ifndef USER_RESOLV_H
#define USER_RESOLV_H

#include "netinet/in.h"

#define MAXNS 3

struct __res_state {
    int nscount;
    struct sockaddr_in nsaddr_list[MAXNS];
};

extern struct __res_state _res;

int res_init(void);

#endif /* USER_RESOLV_H */
