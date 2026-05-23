#ifndef DISPLAY_H
#define DISPLAY_H

#include "types.h"
#include "uapi_display.h"

struct process;

typedef struct display_info {
    u32 width;
    u32 height;
    u32 pitch;
    u32 bpp;
    u32 format;
} display_info_t;

int display_available(void);
int display_acquire(struct process* owner);
void display_release(struct process* owner);
int display_get_info(display_info_t* out);
int display_map(struct process* owner, sys_display_map_info_t* out);
int display_present_page(struct process* owner, sys_display_present_page_t* req);
int display_fill(struct process* owner, u32 x, u32 y, u32 w, u32 h, u32 color);
int display_blit(struct process* owner, u32 x, u32 y, u32 w, u32 h, const u32* pixels);
int display_blit_stride(struct process* owner, u32 x, u32 y, u32 w, u32 h,
                        u32 pitch_pixels, const u32* pixels);

#endif /* DISPLAY_H */
