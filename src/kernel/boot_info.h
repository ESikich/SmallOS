#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include "types.h"

#define BOOT_INFO_PHYS 0x90000u
#define BOOT_FONT_PHYS 0x91000u

#define BOOT_FB_FORMAT_UNKNOWN 0u
#define BOOT_FB_FORMAT_XRGB8888 1u

typedef struct {
    u32 framebuffer_phys;
    u32 framebuffer_width;
    u32 framebuffer_height;
    u32 framebuffer_pitch;
    u32 framebuffer_bpp;
    u32 framebuffer_format;
    u32 framebuffer_valid;
} boot_info_t;

static inline const boot_info_t* boot_info_get(void) {
    return (const boot_info_t*)BOOT_INFO_PHYS;
}

#endif /* BOOT_INFO_H */
