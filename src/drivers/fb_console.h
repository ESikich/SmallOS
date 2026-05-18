#ifndef FB_CONSOLE_H
#define FB_CONSOLE_H

#define FB_CONSOLE_VIRT_BASE 0xD0000000u

int fb_console_init(void);
int fb_console_info(unsigned int* width, unsigned int* height,
                    unsigned int* pitch, unsigned int* bpp);
int fb_console_fill(unsigned int x, unsigned int y, unsigned int w,
                    unsigned int h, unsigned int color);
int fb_console_blit(unsigned int x, unsigned int y, unsigned int w,
                    unsigned int h, const unsigned int* pixels);
int fb_console_blit_stride(unsigned int x, unsigned int y, unsigned int w,
                           unsigned int h, unsigned int pitch_pixels,
                           const unsigned int* pixels);

#endif /* FB_CONSOLE_H */
