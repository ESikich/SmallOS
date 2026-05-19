#ifndef FRACTINT_SMALLOS_STDLIB_H
#define FRACTINT_SMALLOS_STDLIB_H

#include_next <stdlib.h>

#ifndef RAND_MAX
#define RAND_MAX 32767
#endif

int rand(void);
void srand(unsigned int seed);
int abs(int value);
double atof(const char* nptr);
long atol(const char* nptr);

#endif
