#include "stdio.h"

int setvbuf(FILE* stream, char* buf, int mode, size_t size) {
    (void)stream;
    (void)buf;
    (void)mode;
    (void)size;
    return 0;
}

void setbuf(FILE* stream, char* buf) {
    (void)setvbuf(stream, buf, buf ? _IOFBF : _IONBF, BUFSIZ);
}

void rewind(FILE* stream) {
    fseek(stream, 0, SEEK_SET);
    clearerr(stream);
}

int putc(int c, FILE* stream) {
    return fputc(c, stream);
}

int putw(int w, FILE* stream) {
    unsigned char bytes[4];
    bytes[0] = (unsigned char)(w & 0xff);
    bytes[1] = (unsigned char)((w >> 8) & 0xff);
    bytes[2] = (unsigned char)((w >> 16) & 0xff);
    bytes[3] = (unsigned char)((w >> 24) & 0xff);
    return fwrite(bytes, 1, 4, stream) == 4 ? 0 : EOF;
}

int getw(FILE* stream) {
    unsigned char bytes[4];
    if (fread(bytes, 1, 4, stream) != 4) {
        return EOF;
    }
    return (int)bytes[0] |
           ((int)bytes[1] << 8) |
           ((int)bytes[2] << 16) |
           ((int)bytes[3] << 24);
}
