#ifndef FRACTINT_SMALLOS_STRINGS_H
#define FRACTINT_SMALLOS_STRINGS_H

#include <string.h>

void bcopy(const void* src, void* dst, size_t len);
void bzero(void* dst, size_t len);
int bcmp(const void* a, const void* b, size_t len);
int strncasecmp(const char* a, const char* b, size_t n);

#endif
