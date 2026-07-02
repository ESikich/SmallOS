#ifndef USER_SYS_PARAM_H
#define USER_SYS_PARAM_H

#ifndef MAXPATHLEN
#define MAXPATHLEN 128
#endif

#ifndef PATH_MAX
#define PATH_MAX MAXPATHLEN
#endif

#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif

#ifndef howmany
#define howmany(x, y) (((x) + ((y) - 1)) / (y))
#endif

#endif /* USER_SYS_PARAM_H */
