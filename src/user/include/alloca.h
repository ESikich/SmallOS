#ifndef USER_ALLOCA_H
#define USER_ALLOCA_H

#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif

#endif
