#include "image_bmp.h"

static unsigned int rd16(const unsigned char* p) {
    return (unsigned int)p[0] | ((unsigned int)p[1] << 8);
}

static unsigned int rd32(const unsigned char* p) {
    return (unsigned int)p[0] |
           ((unsigned int)p[1] << 8) |
           ((unsigned int)p[2] << 16) |
           ((unsigned int)p[3] << 24);
}

static int rd32s(const unsigned char* p) {
    return (int)rd32(p);
}

static unsigned int pixel_at(const bmp_image_t* bmp, unsigned int x, unsigned int y) {
    unsigned int src_y = bmp->top_down ? y : (bmp->height - 1u - y);
    unsigned int row = bmp->pixel_offset + src_y * bmp->row_stride;
    unsigned int off;

    if (x >= bmp->width || y >= bmp->height || row >= bmp->size) {
        return 0;
    }

    if (bmp->bpp == 24u) {
        off = row + x * 3u;
        if (off + 2u >= bmp->size) return 0;
        return ((unsigned int)bmp->data[off + 2u] << 16) |
               ((unsigned int)bmp->data[off + 1u] << 8) |
               (unsigned int)bmp->data[off];
    }

    if (bmp->bpp == 32u) {
        off = row + x * 4u;
        if (off + 2u >= bmp->size) return 0;
        return ((unsigned int)bmp->data[off + 2u] << 16) |
               ((unsigned int)bmp->data[off + 1u] << 8) |
               (unsigned int)bmp->data[off];
    }

    if (bmp->bpp == 8u) {
        off = row + x;
        if (off >= bmp->size) return 0;
        return bmp->palette[bmp->data[off]];
    }

    return 0;
}

int bmp_parse(const unsigned char* data, unsigned int size, bmp_image_t* out) {
    unsigned int pixel_offset;
    unsigned int dib_size;
    int raw_width;
    int raw_height;
    unsigned int width;
    unsigned int height;
    unsigned int bpp;
    unsigned int row_bits;
    unsigned int row_stride;

    if (!data || !out || size < 54u || data[0] != 'B' || data[1] != 'M') {
        return BMP_ERR_FORMAT;
    }

    pixel_offset = rd32(data + 10);
    dib_size = rd32(data + 14);
    if (dib_size < 40u || dib_size > size - 14u) {
        return BMP_ERR_UNSUPPORTED;
    }

    raw_width = rd32s(data + 18);
    raw_height = rd32s(data + 22);
    bpp = rd16(data + 28);

    if (rd16(data + 26) != 1u || rd32(data + 30) != 0u ||
        (bpp != 8u && bpp != 24u && bpp != 32u)) {
        return BMP_ERR_UNSUPPORTED;
    }
    if (raw_width == 0 || raw_height == 0) {
        return BMP_ERR_FORMAT;
    }
    if (raw_width > 8192 || raw_width < -8192 ||
        raw_height > 8192 || raw_height < -8192) {
        return BMP_ERR_TOO_LARGE;
    }
    width = raw_width < 0 ? (unsigned int)(-raw_width) : (unsigned int)raw_width;
    height = raw_height < 0 ? (unsigned int)(-raw_height) : (unsigned int)raw_height;

    row_bits = width * bpp;
    row_stride = ((row_bits + 31u) / 32u) * 4u;
    if (pixel_offset >= size || height > 0xFFFFFFFFu / row_stride ||
        row_stride * height > size - pixel_offset) {
        return BMP_ERR_TRUNCATED;
    }

    memset(out, 0, sizeof(*out));
    out->data = data;
    out->size = size;
    out->width = width;
    out->height = height;
    out->bpp = bpp;
    out->row_stride = row_stride;
    out->pixel_offset = pixel_offset;
    out->top_down = raw_height < 0;

    if (bpp == 8u) {
        unsigned int colors_used = rd32(data + 46);
        unsigned int palette_count = colors_used ? colors_used : 256u;
        unsigned int palette_offset = 14u + dib_size;

        if (palette_count > 256u || palette_offset > size ||
            palette_count > (size - palette_offset) / 4u) {
            return BMP_ERR_TRUNCATED;
        }

        for (unsigned int i = 0; i < palette_count; i++) {
            unsigned int off = palette_offset + i * 4u;
            out->palette[i] = ((unsigned int)data[off + 2u] << 16) |
                              ((unsigned int)data[off + 1u] << 8) |
                              (unsigned int)data[off];
        }
    }

    return BMP_OK;
}

int bmp_decode_row_xrgb8888(const bmp_image_t* bmp,
                            unsigned int y,
                            unsigned int* out_pixels,
                            unsigned int out_count) {
    if (!bmp || !out_pixels || y >= bmp->height || out_count < bmp->width) {
        return BMP_ERR_FORMAT;
    }

    for (unsigned int x = 0; x < bmp->width; x++) {
        out_pixels[x] = pixel_at(bmp, x, y);
    }
    return BMP_OK;
}

const char* bmp_error_string(int error) {
    if (error == BMP_ERR_FORMAT) return "not a BMP file";
    if (error == BMP_ERR_UNSUPPORTED) return "unsupported BMP format";
    if (error == BMP_ERR_TRUNCATED) return "BMP data is truncated";
    if (error == BMP_ERR_TOO_LARGE) return "BMP is too large";
    return "BMP decode failed";
}
