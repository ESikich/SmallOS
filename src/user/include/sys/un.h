#ifndef USER_SYS_UN_H
#define USER_SYS_UN_H

#include "uapi_socket.h"

#define UNIX_PATH_MAX 108

struct sockaddr_un {
    sa_family_t sun_family;
    char sun_path[UNIX_PATH_MAX];
};

#endif /* USER_SYS_UN_H */
