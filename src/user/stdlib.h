#ifndef USER_STDLIB_WRAPPER_H
#define USER_STDLIB_WRAPPER_H

#include "user_lib.h"

void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
char* getenv(const char* name);
char* realpath(const char* path, char* resolved_path);
_Noreturn void exit(int code);

#endif
