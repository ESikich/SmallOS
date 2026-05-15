#include "boot_info.h"

static int boot_info_header_valid(const boot_info_t* info) {
    return info->magic == BOOT_INFO_MAGIC &&
           info->version == BOOT_INFO_VERSION &&
           info->size >= sizeof(boot_info_t);
}

static int e820_entry_sane(const boot_e820_entry_t* ent) {
    if (ent->length == 0) {
        return 0;
    }

    if (ent->base + ent->length < ent->base) {
        return 0;
    }

    return 1;
}

int boot_info_validate(void) {
    const boot_info_t* info = boot_info_get();

    if (!boot_info_header_valid(info)) {
        return 0;
    }

    if (info->e820_count > BOOT_E820_MAX) {
        return 0;
    }

    return 1;
}

int boot_info_framebuffer_valid(void) {
    const boot_info_t* info = boot_info_get();

    if (!boot_info_header_valid(info)) {
        return 0;
    }

    return info->framebuffer_valid != 0 &&
           info->framebuffer_phys != 0 &&
           info->framebuffer_width != 0 &&
           info->framebuffer_height != 0 &&
           info->framebuffer_pitch != 0 &&
           info->framebuffer_bpp != 0;
}

int boot_info_e820_valid(void) {
    const boot_info_t* info = boot_info_get();

    if (!boot_info_validate() ||
        info->e820_valid == 0 ||
        info->e820_count == 0 ||
        info->e820_count > BOOT_E820_MAX) {
        return 0;
    }

    for (u32 i = 0; i < info->e820_count; i++) {
        if (!e820_entry_sane(&info->e820[i])) {
            return 0;
        }
    }

    return 1;
}

u32 boot_info_e820_count(void) {
    const boot_info_t* info = boot_info_get();

    if (!boot_info_e820_valid()) {
        return 0;
    }

    return info->e820_count;
}

int boot_info_ramdisk_valid(void) {
    const boot_info_t* info = boot_info_get();

    if (!boot_info_header_valid(info)) {
        return 0;
    }

    return info->ramdisk_valid != 0 &&
           info->ramdisk_phys != 0 &&
           info->ramdisk_size != 0;
}

u32 boot_info_ramdisk_phys(void) {
    const boot_info_t* info = boot_info_get();
    return boot_info_ramdisk_valid() ? info->ramdisk_phys : 0;
}

u32 boot_info_ramdisk_size(void) {
    const boot_info_t* info = boot_info_get();
    return boot_info_ramdisk_valid() ? info->ramdisk_size : 0;
}
