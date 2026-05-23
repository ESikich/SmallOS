#ifndef SMALLOS_WOLF3D_DIR_H
#define SMALLOS_WOLF3D_DIR_H
#include "DOS.H"
struct ffblk {
    char ff_reserved[21];
    char ff_attrib;
    unsigned ff_ftime;
    unsigned ff_fdate;
    long ff_fsize;
    char ff_name[13];
};
int findfirst(const char* pattern, void* out, unsigned attrib);
int findnext(void* out);
#endif
