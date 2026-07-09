#ifndef USER_STDLIB_WRAPPER_H
#define USER_STDLIB_WRAPPER_H

#include "stddef.h"

#define RAND_MAX 2147483647
#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

#ifndef alloca
#define alloca(size) __builtin_alloca(size)
#endif

void* malloc(size_t size);
void  free(void* ptr);
void* realloc(void* ptr, size_t size);
void* calloc(size_t nmemb, size_t size);
void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
char* getenv(const char* name);
int putenv(char* string);
int setenv(const char* name, const char* value, int overwrite);
int unsetenv(const char* name);
int clearenv(void);
char* mktemp(char* template);
int mkstemp(char* template);
char* mkdtemp(char* template);
extern char** environ;
char* realpath(const char* path, char* resolved_path);
int atoi(const char* nptr);
double atof(const char* nptr);
long atol(const char* nptr);
long long atoll(const char* nptr);
int abs(int x);
void srand(unsigned int seed);
int rand(void);
long int strtol(const char* nptr, char** endptr, int base);
unsigned long int strtoul(const char* nptr, char** endptr, int base);
long long int strtoll(const char* nptr, char** endptr, int base);
unsigned long long int strtoull(const char* nptr, char** endptr, int base);
float strtof(const char* nptr, char** endptr);
long double strtold(const char* nptr, char** endptr);
double strtod(const char* nptr, char** endptr);
long double ldexpl(long double x, int exp);
int system(const char* command);
int atexit(void (*function)(void));
__attribute__((noreturn)) void exit(int code);

#endif
