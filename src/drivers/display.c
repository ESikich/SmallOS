#include "display.h"
#include "fb_console.h"
#include "terminal.h"
#include "uapi_display.h"
#include "../kernel/process.h"

static process_t* s_display_owner = 0;

int display_available(void) {
    unsigned int width = 0;
    unsigned int height = 0;

    return fb_console_info(&width, &height, 0, 0) &&
           width > 0 && height > 0;
}

int display_acquire(process_t* owner) {
    if (!owner) {
        return 0;
    }
    if (!display_available()) {
        return 0;
    }
    if (s_display_owner && s_display_owner != owner) {
        return 0;
    }

    s_display_owner = owner;
    return 1;
}

void display_release(process_t* owner) {
    if (!s_display_owner || (owner && s_display_owner != owner)) {
        return;
    }

    s_display_owner = 0;
    terminal_clear();
}

int display_get_info(display_info_t* out) {
    unsigned int width = 0;
    unsigned int height = 0;
    unsigned int pitch = 0;
    unsigned int bpp = 0;

    if (!out || !fb_console_info(&width, &height, &pitch, &bpp)) {
        return 0;
    }

    out->width = width;
    out->height = height;
    out->pitch = pitch;
    out->bpp = bpp;
    out->format = SYS_DISPLAY_FORMAT_XRGB8888;
    return 1;
}

int display_fill(process_t* owner, u32 x, u32 y, u32 w, u32 h, u32 color) {
    if (!owner || s_display_owner != owner) {
        return 0;
    }
    return fb_console_fill(x, y, w, h, color);
}

int display_blit(process_t* owner, u32 x, u32 y, u32 w, u32 h, const u32* pixels) {
    if (!owner || s_display_owner != owner) {
        return 0;
    }
    return fb_console_blit(x, y, w, h, pixels);
}
