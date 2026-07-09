#ifndef USER_WCHAR_H
#define USER_WCHAR_H

#include "stddef.h"

#ifndef WEOF
#define WEOF ((wint_t)-1)
#endif

typedef unsigned int wint_t;

size_t mbstowcs(wchar_t* dest, const char* src, size_t n);

#endif
