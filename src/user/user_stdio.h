#ifndef USER_STDIO_H
#define USER_STDIO_H

#include <stdarg.h>

#include "user_lib.h"

typedef struct u_file_stream {
    int fd;
    int readable;
    int writable;
    int is_console;
    int has_unget;
    int unget_ch;
} FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

#ifndef EOF
#define EOF (-1)
#endif

FILE* fopen(const char* path, const char* mode);
FILE* fdopen(int fildes, const char* mode);
FILE* freopen(const char* path, const char* mode, FILE* stream);
int fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int fgetc(FILE* stream);
char* fgets(char* s, int size, FILE* stream);
int getc(FILE* stream);
int getchar(void);
char* gets(char* s);
int ungetc(int c, FILE* stream);
int fflush(FILE* stream);
int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
int fputc(int c, FILE* stream);
int fputs(const char* s, FILE* stream);
int putchar(int c);
int puts(const char* s);

int printf(const char* format, ...);
int fprintf(FILE* stream, const char* format, ...);
int sprintf(char* str, const char* format, ...);
int snprintf(char* str, size_t size, const char* format, ...);
int asprintf(char** strp, const char* format, ...);
int dprintf(int fd, const char* format, ...);
int vprintf(const char* format, va_list ap);
int vfprintf(FILE* stream, const char* format, va_list ap);
int vsprintf(char* str, const char* format, va_list ap);
int vsnprintf(char* str, size_t size, const char* format, va_list ap);
int vasprintf(char** strp, const char* format, va_list ap);
int vdprintf(int fd, const char* format, va_list ap);

void perror(const char* s);

char* strcat(char* dest, const char* src);
int strcmp(const char* a, const char* b);
int strncmp(const char* a, const char* b, size_t n);
char* strchr(const char* s, int c);
char* strrchr(const char* s, int c);
char* strcpy(char* dest, const char* src);
char* strncpy(char* dest, const char* src, size_t n);
void* memcpy(void* dest, const void* src, size_t n);
void* memmove(void* dest, const void* src, size_t n);
void* memset(void* s, int c, size_t n);
void* memchr(const void* s, int c, size_t n);
char* strdup(const char* s);
size_t strlen(const char* s);
int strcasecmp(const char* a, const char* b);
int stricmp(const char* a, const char* b);
int strnicmp(const char* a, const char* b, size_t n);
char* strstr(const char* haystack, const char* needle);
char* strpbrk(const char* s, const char* accept);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char* strtok(char* s, const char* delim);
char* strtok_r(char* s, const char* delim, char** saveptr);
void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
void* bsearch(const void* key, const void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*));
char* strerror(int errnum);

int atoi(const char* nptr);
long int strtol(const char* nptr, char** endptr, int base);
unsigned long int strtoul(const char* nptr, char** endptr, int base);
long long int strtoll(const char* nptr, char** endptr, int base);
unsigned long long int strtoull(const char* nptr, char** endptr, int base);
float strtof(const char* nptr, char** endptr);
long double strtold(const char* nptr, char** endptr);
double strtod(const char* nptr, char** endptr);
long double ldexpl(long double x, int exp);
void exit(int);
char* getenv(const char* name);

void* dlopen(const char* filename, int flag);
const char* dlerror(void);
void* dlsym(void* handle, char* symbol);
int dlclose(void* handle);

#endif
