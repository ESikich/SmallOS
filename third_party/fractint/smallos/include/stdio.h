#ifndef FRACTINT_SMALLOS_STDIO_H
#define FRACTINT_SMALLOS_STDIO_H

#include_next <stdio.h>

#ifndef _IOFBF
#define _IOFBF 0
#endif

int setvbuf(FILE* stream, char* buf, int mode, size_t size);
void rewind(FILE* stream);

#endif
