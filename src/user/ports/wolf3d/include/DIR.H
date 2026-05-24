#ifndef SMALLOS_WOLF3D_DIR_H
#define SMALLOS_WOLF3D_DIR_H
#include "DOS.H"
int findfirst(const char* pattern, void* out, unsigned attrib);
int findnext(void* out);
#endif
