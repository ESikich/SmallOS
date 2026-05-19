#ifndef FRACTINT_SMALLOS_SYS_STAT_H
#define FRACTINT_SMALLOS_SYS_STAT_H

#include_next <sys/stat.h>

#ifndef S_IREAD
#define S_IREAD S_IRUSR
#endif
#ifndef S_IWRITE
#define S_IWRITE S_IWUSR
#endif

#endif
