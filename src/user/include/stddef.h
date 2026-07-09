#ifndef USER_STDDEF_WRAPPER_H
#define USER_STDDEF_WRAPPER_H

typedef unsigned int size_t;
typedef int ptrdiff_t;

#ifndef __WCHAR_TYPE__
#define __WCHAR_TYPE__ int
#endif
#ifndef _GCC_WCHAR_T
#define __WCHAR_T__
#define _WCHAR_T
#define __WCHAR_T
#define _WCHAR_T_
#define _WCHAR_T_DEFINED_
#define _WCHAR_T_DEFINED
#define _GCC_WCHAR_T
#define _WCHAR_T_DECLARED
#define __DEFINED_wchar_t
typedef __WCHAR_TYPE__ wchar_t;
#endif

#ifndef NULL
#define NULL ((void*)0)
#endif

#ifndef offsetof
#define offsetof(type, member) __builtin_offsetof(type, member)
#endif

#endif
