#ifndef USER_SYS_SENDFILE_H
#define USER_SYS_SENDFILE_H

#include "types.h"

ssize_t sendfile(int out_fd, int in_fd, off_t* offset, size_t count);

#endif /* USER_SYS_SENDFILE_H */
