#ifndef USER_DIR_H
#define USER_DIR_H

#include "smallos_dos.h"

int findfirst(const char* pattern, void* out, unsigned attrib);
int findnext(void* out);

#endif /* USER_DIR_H */
