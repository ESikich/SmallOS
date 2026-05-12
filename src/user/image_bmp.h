#ifndef IMAGE_BMP_H
#define IMAGE_BMP_H

#include "user_lib.h"

enum {
    BMP_OK = 0,
    BMP_ERR_FORMAT = -1,
    BMP_ERR_UNSUPPORTED = -2,
    BMP_ERR_TRUNCATED = -3,
    BMP_ERR_TOO_LARGE = -4
};

typedef struct bmp_image {
    const unsigned char* data;
    unsigned int size;
    unsigned int width;
    unsigned int height;
    unsigned int bpp;
    unsigned int row_stride;
    unsigned int pixel_offset;
    int top_down;
    unsigned int palette[256];
} bmp_image_t;

int bmp_parse(const unsigned char* data, unsigned int size, bmp_image_t* out);
int bmp_decode_row_xrgb8888(const bmp_image_t* bmp,
                            unsigned int y,
                            unsigned int* out_pixels,
                            unsigned int out_count);
const char* bmp_error_string(int error);

#endif /* IMAGE_BMP_H */
