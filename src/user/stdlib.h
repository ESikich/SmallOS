#ifndef USER_STDLIB_WRAPPER_H
#define USER_STDLIB_WRAPPER_H

#include "user_lib.h"

void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
char* getenv(const char* name);
char* realpath(const char* path, char* resolved_path);
int atoi(const char* nptr);
long long atoll(const char* nptr);
long int strtol(const char* nptr, char** endptr, int base);
long long int strtoll(const char* nptr, char** endptr, int base);
__attribute__((noreturn)) void exit(int code);

#endif
