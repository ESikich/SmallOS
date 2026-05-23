#ifndef FB_CONSOLE_H
#define FB_CONSOLE_H

#define FB_CONSOLE_VIRT_BASE 0xD0000000u
#define FB_CONSOLE_USER_BASE 0xB0000000u

#include "../kernel/types.h"

int fb_console_init(void);
int fb_console_info(unsigned int* width, unsigned int* height,
                    unsigned int* pitch, unsigned int* bpp);
int fb_console_map_user(u32* pd, unsigned int* out_base,
                        unsigned int* out_page_bytes,
                        unsigned int* out_page_count,
                        unsigned int* out_draw_page);
int fb_console_present_page(unsigned int page, unsigned int* out_next_page);
int fb_console_fill(unsigned int x, unsigned int y, unsigned int w,
                    unsigned int h, unsigned int color);
int fb_console_blit(unsigned int x, unsigned int y, unsigned int w,
                    unsigned int h, const unsigned int* pixels);
int fb_console_blit_stride(unsigned int x, unsigned int y, unsigned int w,
                           unsigned int h, unsigned int pitch_pixels,
                           const unsigned int* pixels);
void fb_console_reset_scanout(void);

#endif /* FB_CONSOLE_H */
