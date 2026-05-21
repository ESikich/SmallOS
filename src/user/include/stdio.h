#ifndef USER_STDIO_H
#define USER_STDIO_H

#include <stdarg.h>

#include "stddef.h"

#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

#define _IOFBF 0
#define _IOLBF 1
#define _IONBF 2

typedef struct u_file_stream {
    int fd;
    int readable;
    int writable;
    int is_console;
    int has_unget;
    int unget_ch;
    int eof;
    int error;
} FILE;

extern FILE* stdin;
extern FILE* stdout;
extern FILE* stderr;

#ifndef EOF
#define EOF (-1)
#endif

FILE* fopen(const char* path, const char* mode);
FILE* fdopen(int fildes, const char* mode);
FILE* freopen(const char* path, const char* mode, FILE* stream);
int fclose(FILE* stream);
size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream);
size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream);
int fgetc(FILE* stream);
char* fgets(char* s, int size, FILE* stream);
int getc(FILE* stream);
int getchar(void);
char* gets(char* s);
int ungetc(int c, FILE* stream);
int fflush(FILE* stream);
int feof(FILE* stream);
int ferror(FILE* stream);
void clearerr(FILE* stream);
int fseek(FILE* stream, long offset, int whence);
long ftell(FILE* stream);
int fputc(int c, FILE* stream);
int fputs(const char* s, FILE* stream);
int putc(int c, FILE* stream);
int putw(int w, FILE* stream);
int getw(FILE* stream);
int putchar(int c);
int puts(const char* s);
int setvbuf(FILE* stream, char* buf, int mode, size_t size);
void rewind(FILE* stream);

int printf(const char* format, ...);
int fprintf(FILE* stream, const char* format, ...);
int sprintf(char* str, const char* format, ...);
int snprintf(char* str, size_t size, const char* format, ...);
int asprintf(char** strp, const char* format, ...);
int dprintf(int fd, const char* format, ...);
int vprintf(const char* format, va_list ap);
int vfprintf(FILE* stream, const char* format, va_list ap);
int vsprintf(char* str, const char* format, va_list ap);
int vsnprintf(char* str, size_t size, const char* format, va_list ap);
int vasprintf(char** strp, const char* format, va_list ap);
int vdprintf(int fd, const char* format, va_list ap);
int vsscanf(const char* input, const char* fmt, va_list ap);
int sscanf(const char* input, const char* fmt, ...);
int fscanf(FILE* stream, const char* fmt, ...);

void perror(const char* s);

#endif
