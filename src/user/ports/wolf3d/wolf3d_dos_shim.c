#include "wolf3d_port.h"

#include <stdio.h>
#include <string.h>

#include "DIR.H"

static smallos_dos_find_t wolf3d_find;

int wolf3d_argc;
char** wolf3d_argv;
unsigned int wolf3d_reg_ax;
unsigned int wolf3d_reg_bx;
unsigned int wolf3d_reg_cx;
unsigned int wolf3d_reg_dx;
unsigned int wolf3d_reg_di;
unsigned int wolf3d_reg_si;

int findfirst(const char* pattern, void* out, unsigned attrib) {
    char asset_pattern[192];

    if (smallos_dos_findfirst(&wolf3d_find, pattern, (struct ffblk*)out,
                              attrib) == 0) {
        return 0;
    }
    if (strchr(pattern ? pattern : "", '/')) {
        return -1;
    }

    snprintf(asset_pattern, sizeof(asset_pattern), "/usr/share/wolf3d/%s",
             pattern ? pattern : "");
    return smallos_dos_findfirst(&wolf3d_find, asset_pattern,
                                 (struct ffblk*)out, attrib);
}

int findnext(void* out) {
    return smallos_dos_findnext(&wolf3d_find, (struct ffblk*)out);
}
