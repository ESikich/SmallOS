#ifndef USER_STRING_WRAPPER_H
#define USER_STRING_WRAPPER_H

#include "stddef.h"

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
int memcmp(const void* a, const void* b, size_t n);
void* memchr(const void* s, int c, size_t n);
char* strdup(const char* s);
size_t strlen(const char* s);
int strcasecmp(const char* a, const char* b);
int strncasecmp(const char* a, const char* b, size_t n);
int stricmp(const char* a, const char* b);
int strnicmp(const char* a, const char* b, size_t n);
char* strstr(const char* haystack, const char* needle);
char* strpbrk(const char* s, const char* accept);
size_t strspn(const char* s, const char* accept);
size_t strcspn(const char* s, const char* reject);
char* strtok(char* s, const char* delim);
char* strtok_r(char* s, const char* delim, char** saveptr);
char* strerror(int errnum);
void bcopy(const void* src, void* dst, size_t len);
void bzero(void* dst, size_t len);
int bcmp(const void* a, const void* b, size_t len);

#endif
