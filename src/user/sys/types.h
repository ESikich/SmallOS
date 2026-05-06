#ifndef USER_SYS_TYPES_H
#define USER_SYS_TYPES_H

#include "../stddef.h"
#include "../stdint.h"
#include "../time.h"

#ifndef USER_SSIZE_T_DEFINED
typedef int ssize_t;
#define USER_SSIZE_T_DEFINED
#endif

typedef int off_t;
#ifndef USER_MODE_T_DEFINED
typedef unsigned int mode_t;
#define USER_MODE_T_DEFINED
#endif
typedef unsigned int uid_t;
typedef unsigned int gid_t;
typedef int pid_t;

#endif /* USER_SYS_TYPES_H */
