#ifndef BOOT_INFO_H
#define BOOT_INFO_H

#include "types.h"

#define BOOT_INFO_PHYS 0x90000u
#define BOOT_FONT_PHYS 0x91000u

#define BOOT_INFO_MAGIC 0x534D4F53u /* SMOS */
#define BOOT_INFO_VERSION 3u
#define BOOT_E820_MAX 32u

#define BOOT_FB_FORMAT_UNKNOWN 0u
#define BOOT_FB_FORMAT_XRGB8888 1u

typedef struct {
    u64 base;
    u64 length;
    u32 type;
    u32 attr;
} boot_e820_entry_t;

typedef struct {
    u32 magic;
    u32 version;
    u32 size;

    u32 framebuffer_phys;
    u32 framebuffer_width;
    u32 framebuffer_height;
    u32 framebuffer_pitch;
    u32 framebuffer_bpp;
    u32 framebuffer_format;
    u32 framebuffer_valid;

    u32 e820_count;
    u32 e820_valid;
    boot_e820_entry_t e820[BOOT_E820_MAX];

    u32 ramdisk_phys;
    u32 ramdisk_size;
    u32 ramdisk_valid;
    u32 reserved;
} boot_info_t;

static inline const boot_info_t* boot_info_get(void) {
    return (const boot_info_t*)BOOT_INFO_PHYS;
}

int boot_info_validate(void);
int boot_info_framebuffer_valid(void);
int boot_info_e820_valid(void);
u32 boot_info_e820_count(void);
int boot_info_ramdisk_valid(void);
u32 boot_info_ramdisk_phys(void);
u32 boot_info_ramdisk_size(void);

#endif /* BOOT_INFO_H */
