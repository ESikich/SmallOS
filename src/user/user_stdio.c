#define USER_LIB_NO_STRING_IMPL
#include "user_stdio.h"

static FILE s_stdin = { 0, 1, 0, 1, 0, 0 };
static FILE s_stdout = { 1, 0, 1, 1, 0, 0 };
static FILE s_stderr = { 2, 0, 1, 1, 0, 0 };

FILE* stdin = &s_stdin;
FILE* stdout = &s_stdout;
FILE* stderr = &s_stderr;

static int mode_allows_read(const char* mode) {
    return mode && (strchr(mode, 'r') || strchr(mode, '+'));
}

static int mode_allows_write(const char* mode) {
    return mode && (strchr(mode, 'w') || strchr(mode, 'a') || strchr(mode, '+'));
}

static int fd_from_mode(const char* mode, const char* path) {
    if (mode_allows_write(mode)) {
        return sys_open_write(path);
    }
    return sys_open(path);
}

static FILE* file_from_fd(int fd, const char* mode) {
    if (fd < 0) return 0;
    FILE* f = (FILE*)malloc(sizeof(FILE));
    if (!f) {
        sys_close(fd);
        return 0;
    }
    f->fd = fd;
    f->readable = mode_allows_read(mode);
    f->writable = mode_allows_write(mode);
    f->is_console = 0;
    f->has_unget = 0;
    f->unget_ch = 0;
    return f;
}

static FILE* console_stream(FILE* base, int fd) {
    base->fd = fd;
    base->readable = (fd == 0);
    base->writable = (fd != 0);
    base->is_console = 1;
    base->has_unget = 0;
    base->unget_ch = 0;
    return base;
}

static int stream_write_raw(FILE* stream, const void* ptr, size_t size) {
    if (!stream || !stream->writable) return -1;
    if (stream->is_console) {
        const char* p = (const char*)ptr;
        for (size_t i = 0; i < size; i++) {
            sys_putc(p[i]);
        }
        return (int)size;
    }
    return u_writefd(stream->fd, (const char*)ptr, (uint32_t)size);
}

static int stream_read_raw(FILE* stream, void* ptr, size_t size) {
    if (!stream || !stream->readable) return -1;
    if (stream->is_console) {
        return sys_read((char*)ptr, (uint32_t)size);
    }
    return sys_fread(stream->fd, (char*)ptr, (uint32_t)size);
}

FILE* fopen(const char* path, const char* mode) {
    return file_from_fd(fd_from_mode(mode, path), mode);
}

FILE* fdopen(int fildes, const char* mode) {
    return file_from_fd(fildes, mode);
}

FILE* freopen(const char* path, const char* mode, FILE* stream) {
    if (!stream) return 0;
    if (stream->fd >= 0 && !stream->is_console) {
        sys_close(stream->fd);
    }
    stream->fd = fd_from_mode(mode, path);
    stream->readable = mode_allows_read(mode);
    stream->writable = mode_allows_write(mode);
    stream->is_console = 0;
    stream->has_unget = 0;
    stream->unget_ch = 0;
    return stream->fd < 0 ? 0 : stream;
}

int fclose(FILE* stream) {
    if (!stream) return -1;
    if (stream->is_console) return 0;
    int r = sys_close(stream->fd);
    free(stream);
    return r;
}

size_t fread(void* ptr, size_t size, size_t nmemb, FILE* stream) {
    size_t want = size * nmemb;
    if (want == 0) return 0;
    int r = stream_read_raw(stream, ptr, want);
    if (r < 0) return 0;
    return (size_t)r / size;
}

size_t fwrite(const void* ptr, size_t size, size_t nmemb, FILE* stream) {
    size_t want = size * nmemb;
    if (want == 0) return 0;
    int r = stream_write_raw(stream, ptr, want);
    if (r < 0) return 0;
    return (size_t)r / size;
}

int fgetc(FILE* stream) {
    unsigned char ch = 0;
    if (!stream) return EOF;
    if (stream->has_unget) {
        stream->has_unget = 0;
        return stream->unget_ch;
    }
    if (stream_read_raw(stream, &ch, 1) != 1) return EOF;
    return (int)ch;
}

char* fgets(char* s, int size, FILE* stream) {
    if (!s || size <= 0 || !stream) return 0;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) break;
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return 0;
    s[i] = '\0';
    return s;
}

int getc(FILE* stream) {
    return fgetc(stream);
}

int getchar(void) {
    return fgetc(stdin);
}

char* gets(char* s) {
    if (!s) return 0;
    int i = 0;
    while (1) {
        int c = getchar();
        if (c == EOF || c == '\n') break;
        s[i++] = (char)c;
    }
    s[i] = '\0';
    return s;
}

int ungetc(int c, FILE* stream) {
    if (!stream || c == EOF) return EOF;
    stream->has_unget = 1;
    stream->unget_ch = c & 0xFF;
    return c;
}

int fflush(FILE* stream) {
    (void)stream;
    return 0;
}

int fputc(int c, FILE* stream) {
    unsigned char ch = (unsigned char)c;
    if (stream_write_raw(stream, &ch, 1) != 1) return EOF;
    return c;
}

int fputs(const char* s, FILE* stream) {
    size_t len = strlen(s);
    return stream_write_raw(stream, s, len) == (int)len ? 0 : EOF;
}

int fseek(FILE* stream, long offset, int whence) {
    if (!stream || stream->is_console) return -1;
    return sys_lseek(stream->fd, (int)offset, whence);
}

long ftell(FILE* stream) {
    if (!stream || stream->is_console) return -1;
    return (long)sys_lseek(stream->fd, 0, SEEK_CUR);
}

int putchar(int c) {
    sys_putc((char)c);
    return c;
}

int puts(const char* s) {
    u_puts(s);
    sys_putc('\n');
    return 0;
}

static void append_char(char** out, size_t* left, char c, size_t* count) {
    if (*left > 1) {
        **out = c;
        (*out)++;
        (*left)--;
    }
    (*count)++;
}

static void append_str(char** out, size_t* left, const char* s, size_t* count) {
    if (!s) s = "(null)";
    while (*s) {
        append_char(out, left, *s++, count);
    }
}

static void append_uint(char** out, size_t* left, unsigned long value, unsigned base,
                        int uppercase, size_t* count) {
    char tmp[32];
    unsigned int i = 0;
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    if (value == 0) {
        append_char(out, left, '0', count);
        return;
    }
    while (value > 0 && i < sizeof(tmp)) {
        tmp[i++] = digits[value % base];
        value /= base;
    }
    while (i > 0) {
        append_char(out, left, tmp[--i], count);
    }
}

static void append_uint64_hex_parts(char** out, size_t* left,
                                    unsigned long hi, unsigned long lo,
                                    int uppercase, size_t* count) {
    char tmp[16];
    unsigned int i = 0;
    const char* digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";

    if (hi == 0u && lo == 0u) {
        append_char(out, left, '0', count);
        return;
    }

    while (hi != 0u || lo != 0u) {
        unsigned int nibble = (unsigned int)(lo & 0xFu);
        tmp[i++] = digits[nibble];
        lo = (lo >> 4) | (hi << 28);
        hi >>= 4;
    }

    while (i > 0u) {
        append_char(out, left, tmp[--i], count);
    }
}

static void append_uint64_dec_parts(char** out, size_t* left,
                                    unsigned long hi, unsigned long lo,
                                    size_t* count) {
    char tmp[32];
    unsigned int i = 0;

    if (hi == 0u && lo == 0u) {
        append_char(out, left, '0', count);
        return;
    }

    while (hi != 0u || lo != 0u) {
        unsigned long q_hi = 0u;
        unsigned long q_lo = 0u;
        unsigned long rem = 0u;

        for (int bit = 63; bit >= 0; bit--) {
            unsigned long bitval;
            unsigned long q_bit;

            if (bit >= 32) {
                bitval = (hi >> (bit - 32)) & 1u;
            } else {
                bitval = (lo >> bit) & 1u;
            }

            rem = (rem << 1) | bitval;
            if (rem >= 10u) {
                rem -= 10u;
                q_bit = 1u;
            } else {
                q_bit = 0u;
            }

            q_hi = (q_hi << 1) | (q_lo >> 31);
            q_lo = (q_lo << 1) | q_bit;
        }

        tmp[i++] = (char)('0' + rem);
        hi = q_hi;
        lo = q_lo;
    }

    while (i > 0u) {
        append_char(out, left, tmp[--i], count);
    }
}

static void append_int(char** out, size_t* left, long value, size_t* count) {
    if (value < 0) {
        append_char(out, left, '-', count);
        append_uint(out, left, (unsigned long)(-value), 10, 0, count);
    } else {
        append_uint(out, left, (unsigned long)value, 10, 0, count);
    }
}

static void append_int64(char** out, size_t* left, long long value, size_t* count) {
    union {
        unsigned long long full;
        struct {
            unsigned long lo;
            unsigned long hi;
        } parts;
    } u;

    u.full = (unsigned long long)value;
    if (value < 0) {
        unsigned long carry = (u.parts.lo == 0u) ? 1u : 0u;
        u.parts.lo = ~u.parts.lo + 1u;
        u.parts.hi = ~u.parts.hi + carry;
        append_char(out, left, '-', count);
    }
    append_uint64_dec_parts(out, left, u.parts.hi, u.parts.lo, count);
}

static const char* consume_decimal_width(const char* fmt, va_list* ap) {
    if (*fmt == '*') {
        (void)va_arg(*ap, int);
        return fmt + 1;
    }
    while (*fmt >= '0' && *fmt <= '9') {
        fmt++;
    }
    return fmt;
}

static int format_into(char* str, size_t size, const char* fmt, va_list ap) {
    char* out = str;
    size_t left = size;
    size_t count = 0;
    if (left > 0) *out = '\0';
    while (*fmt) {
        if (*fmt != '%') {
            append_char(&out, &left, *fmt++, &count);
            continue;
        }
        fmt++;
        if (*fmt == '%') {
            append_char(&out, &left, '%', &count);
            fmt++;
            continue;
        }
        while (*fmt == '-' || *fmt == '+' || *fmt == ' ' || *fmt == '#'
            || *fmt == '0' || *fmt == '\'') {
            fmt++;
        }
        fmt = consume_decimal_width(fmt, &ap);
        if (*fmt == '.') {
            fmt++;
            fmt = consume_decimal_width(fmt, &ap);
        }
        int long_long = 0;
        int long_count = 0;
        int short_count = 0;
        int size_t_mod = 0;
        int ptrdiff_mod = 0;
        int intmax_mod = 0;
        while (*fmt == 'l' || *fmt == 'z' || *fmt == 'h' || *fmt == 't'
            || *fmt == 'j' || *fmt == 'L') {
            if (*fmt == 'l') {
                if (fmt[1] == 'l') {
                    long_long = 1;
                    fmt += 2;
                } else {
                    long_count = 1;
                    fmt++;
                }
            } else if (*fmt == 'h') {
                if (fmt[1] == 'h') {
                    short_count = 2;
                    fmt += 2;
                } else {
                    short_count = 1;
                    fmt++;
                }
            } else if (*fmt == 'z') {
                size_t_mod = 1;
                fmt++;
            } else if (*fmt == 't') {
                ptrdiff_mod = 1;
                fmt++;
            } else if (*fmt == 'j') {
                intmax_mod = 1;
                fmt++;
            } else {
                fmt++;
            }
        }
        switch (*fmt++) {
            case 's':
                append_str(&out, &left, va_arg(ap, const char*), &count);
                break;
            case 'c':
                append_char(&out, &left, (char)va_arg(ap, int), &count);
                break;
            case 'd':
            case 'i':
                if (long_long || intmax_mod) {
                    append_int64(&out, &left, va_arg(ap, long long), &count);
                } else if (long_count || ptrdiff_mod) {
                    append_int(&out, &left, va_arg(ap, long), &count);
                } else if (short_count) {
                    append_int(&out, &left, (long)(short)va_arg(ap, int), &count);
                } else {
                    append_int(&out, &left, (long)va_arg(ap, int), &count);
                }
                break;
            case 'u':
                if (long_long || intmax_mod) {
                    union {
                        unsigned long long full;
                        struct {
                            unsigned long lo;
                            unsigned long hi;
                        } parts;
                    } u;
                    u.full = va_arg(ap, unsigned long long);
                    append_uint64_dec_parts(&out, &left, u.parts.hi, u.parts.lo, &count);
                } else if (long_count || size_t_mod || ptrdiff_mod) {
                    append_uint(&out, &left, (unsigned long)va_arg(ap, unsigned long), 10, 0, &count);
                } else if (short_count) {
                    append_uint(&out, &left, (unsigned long)(unsigned short)va_arg(ap, unsigned int), 10, 0, &count);
                } else {
                    append_uint(&out, &left, (unsigned long)va_arg(ap, unsigned int), 10, 0, &count);
                }
                break;
            case 'x':
                if (long_long || intmax_mod) {
                    union {
                        unsigned long long full;
                        struct {
                            unsigned long lo;
                            unsigned long hi;
                        } parts;
                    } u;
                    u.full = va_arg(ap, unsigned long long);
                    append_uint64_hex_parts(&out, &left, u.parts.hi, u.parts.lo, 0, &count);
                } else if (long_count || size_t_mod || ptrdiff_mod) {
                    append_uint(&out, &left, (unsigned long)va_arg(ap, unsigned long), 16, 0, &count);
                } else if (short_count) {
                    append_uint(&out, &left, (unsigned long)(unsigned short)va_arg(ap, unsigned int), 16, 0, &count);
                } else {
                    append_uint(&out, &left, (unsigned long)va_arg(ap, unsigned int), 16, 0, &count);
                }
                break;
            case 'X':
                if (long_long || intmax_mod) {
                    union {
                        unsigned long long full;
                        struct {
                            unsigned long lo;
                            unsigned long hi;
                        } parts;
                    } u;
                    u.full = va_arg(ap, unsigned long long);
                    append_uint64_hex_parts(&out, &left, u.parts.hi, u.parts.lo, 1, &count);
                } else if (long_count || size_t_mod || ptrdiff_mod) {
                    append_uint(&out, &left, (unsigned long)va_arg(ap, unsigned long), 16, 1, &count);
                } else if (short_count) {
                    append_uint(&out, &left, (unsigned long)(unsigned short)va_arg(ap, unsigned int), 16, 1, &count);
                } else {
                    append_uint(&out, &left, (unsigned long)va_arg(ap, unsigned int), 16, 1, &count);
                }
                break;
            case 'p':
                append_str(&out, &left, "0x", &count);
                append_uint(&out, &left, (unsigned long)(unsigned int)va_arg(ap, void*), 16, 0, &count);
                break;
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
                (void)va_arg(ap, double);
                append_str(&out, &left, "<float>", &count);
                break;
            default:
                append_char(&out, &left, '%', &count);
                append_char(&out, &left, fmt[-1], &count);
                break;
        }
    }
    if (size > 0) {
        if (left > 0) {
            *out = '\0';
        } else {
            str[size - 1] = '\0';
        }
    }
    return (int)count;
}

int vsnprintf(char* str, size_t size, const char* format, va_list ap) {
    va_list copy;
    va_copy(copy, ap);
    int n = format_into(str, size, format, copy);
    va_end(copy);
    return n;
}

int snprintf(char* str, size_t size, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vsnprintf(str, size, format, ap);
    va_end(ap);
    return n;
}

int vsprintf(char* str, const char* format, va_list ap) {
    return vsnprintf(str, (size_t)-1, format, ap);
}

int sprintf(char* str, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vsprintf(str, format, ap);
    va_end(ap);
    return n;
}

static int vprint_to_fd(int fd, const char* format, va_list ap) {
    char buf[1024];
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int n = vsnprintf(buf, sizeof(buf), format, ap_copy);
    va_end(ap_copy);
    if (n < 0) return n;
    if (n > (int)sizeof(buf) - 1) {
        char* dyn = (char*)malloc((size_t)n + 1u);
        if (!dyn) return -1;
        va_list ap2;
        va_copy(ap2, ap);
        vsnprintf(dyn, (size_t)n + 1u, format, ap2);
        va_end(ap2);
        sys_writefd(fd, dyn, (uint32_t)n);
        free(dyn);
        return n;
    }
    sys_writefd(fd, buf, (uint32_t)n);
    return n;
}

int vprintf(const char* format, va_list ap) {
    return vprint_to_fd(1, format, ap);
}

int printf(const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vprintf(format, ap);
    va_end(ap);
    return n;
}

int vfprintf(FILE* stream, const char* format, va_list ap) {
    if (!stream) return -1;
    char buf[1024];
    va_list ap_copy;
    va_copy(ap_copy, ap);
    int n = vsnprintf(buf, sizeof(buf), format, ap_copy);
    va_end(ap_copy);
    if (n < 0) return n;
    if (n > (int)sizeof(buf) - 1) {
        char* dyn = (char*)malloc((size_t)n + 1u);
        if (!dyn) return -1;
        va_list ap2;
        va_copy(ap2, ap);
        vsnprintf(dyn, (size_t)n + 1u, format, ap2);
        va_end(ap2);
        stream_write_raw(stream, dyn, (size_t)n);
        free(dyn);
        return n;
    }
    stream_write_raw(stream, buf, (size_t)n);
    return n;
}

int fprintf(FILE* stream, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vfprintf(stream, format, ap);
    va_end(ap);
    return n;
}

int vdprintf(int fd, const char* format, va_list ap) {
    return vprint_to_fd(fd, format, ap);
}

int dprintf(int fd, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vdprintf(fd, format, ap);
    va_end(ap);
    return n;
}

int vasprintf(char** strp, const char* format, va_list ap) {
    if (!strp) return -1;
    char tmp[1];
    va_list ap1;
    va_copy(ap1, ap);
    int n = vsnprintf(tmp, 0, format, ap1);
    va_end(ap1);
    if (n < 0) return n;
    char* out = (char*)malloc((size_t)n + 1u);
    if (!out) return -1;
    va_list ap2;
    va_copy(ap2, ap);
    vsnprintf(out, (size_t)n + 1u, format, ap2);
    va_end(ap2);
    *strp = out;
    return n;
}

int asprintf(char** strp, const char* format, ...) {
    va_list ap;
    va_start(ap, format);
    int n = vasprintf(strp, format, ap);
    va_end(ap);
    return n;
}

void perror(const char* s) {
    if (s) {
        u_puts(s);
        u_puts(": ");
    }
    u_puts("error\n");
}

char* strcat(char* dest, const char* src) {
    char* d = dest + strlen(dest);
    while ((*d++ = *src++)) {
    }
    return dest;
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char* a, const char* b, size_t n) {
    while (n > 0 && *a && *a == *b) {
        a++;
        b++;
        n--;
    }
    if (n == 0) return 0;
    return (unsigned char)*a - (unsigned char)*b;
}

char* strchr(const char* s, int c) {
    char ch = (char)c;
    while (*s) {
        if (*s == ch) return (char*)s;
        s++;
    }
    return ch == '\0' ? (char*)s : 0;
}

char* strrchr(const char* s, int c) {
    const char* last = 0;
    char ch = (char)c;
    while (*s) {
        if (*s == ch) last = s;
        s++;
    }
    if (ch == '\0') return (char*)s;
    return (char*)last;
}

char* strcpy(char* dest, const char* src) {
    char* d = dest;
    while ((*d++ = *src++)) {
    }
    return dest;
}

char* strncpy(char* dest, const char* src, size_t n) {
    size_t i = 0;
    for (; i < n && src[i]; i++) {
        dest[i] = src[i];
    }
    for (; i < n; i++) {
        dest[i] = '\0';
    }
    return dest;
}

void* memcpy(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return dest;
}

void* memmove(void* dest, const void* src, size_t n) {
    unsigned char* d = (unsigned char*)dest;
    const unsigned char* s = (const unsigned char*)src;
    if (d < s) {
        for (size_t i = 0; i < n; i++) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = n; i > 0; i--) d[i - 1] = s[i - 1];
    }
    return dest;
}

void* memset(void* s, int c, size_t n) {
    unsigned char* p = (unsigned char*)s;
    for (size_t i = 0; i < n; i++) p[i] = (unsigned char)c;
    return s;
}

void* memchr(const void* s, int c, size_t n) {
    const unsigned char* p = (const unsigned char*)s;
    unsigned char ch = (unsigned char)c;
    for (size_t i = 0; i < n; i++) {
        if (p[i] == ch) {
            return (void*)(p + i);
        }
    }
    return 0;
}

char* strdup(const char* s) {
    size_t n = strlen(s) + 1u;
    char* out = (char*)malloc(n);
    if (!out) return 0;
    memcpy(out, s, n);
    return out;
}

size_t strlen(const char* s) {
    size_t n = 0;
    while (s && s[n]) n++;
    return n;
}

static int c_tolower(int c) {
    if (c >= 'A' && c <= 'Z') return c - 'A' + 'a';
    return c;
}

int strcasecmp(const char* a, const char* b) {
    while (*a && *b) {
        int ca = c_tolower((unsigned char)*a);
        int cb = c_tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }
    return c_tolower((unsigned char)*a) - c_tolower((unsigned char)*b);
}

int stricmp(const char* a, const char* b) {
    return strcasecmp(a, b);
}

int strnicmp(const char* a, const char* b, size_t n) {
    while (n > 0 && *a && *b) {
        int ca = c_tolower((unsigned char)*a);
        int cb = c_tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
        n--;
    }
    if (n == 0) return 0;
    return c_tolower((unsigned char)*a) - c_tolower((unsigned char)*b);
}

size_t strspn(const char* s, const char* accept) {
    size_t n = 0;
    while (*s) {
        const char* a = accept;
        int ok = 0;
        while (*a) {
            if (*s == *a++) {
                ok = 1;
                break;
            }
        }
        if (!ok) break;
        s++;
        n++;
    }
    return n;
}

size_t strcspn(const char* s, const char* reject) {
    size_t n = 0;
    while (*s) {
        const char* r = reject;
        int hit = 0;
        while (*r) {
            if (*s == *r++) {
                hit = 1;
                break;
            }
        }
        if (hit) break;
        s++;
        n++;
    }
    return n;
}

char* strpbrk(const char* s, const char* accept) {
    while (*s) {
        const char* a = accept;
        while (*a) {
            if (*s == *a++) {
                return (char*)s;
            }
        }
        s++;
    }
    return 0;
}

char* strstr(const char* haystack, const char* needle) {
    if (!*needle) return (char*)haystack;
    for (; *haystack; haystack++) {
        const char* h = haystack;
        const char* n = needle;
        while (*h && *n && *h == *n) {
            h++;
            n++;
        }
        if (!*n) return (char*)haystack;
    }
    return 0;
}

char* strtok(char* s, const char* delim) {
    static char* save;
    if (!s) s = save;
    if (!s) return 0;
    s += strspn(s, delim);
    if (*s == '\0') {
        save = 0;
        return 0;
    }
    char* end = s + strcspn(s, delim);
    if (*end) {
        *end++ = '\0';
        save = end;
    } else {
        save = 0;
    }
    return s;
}

void qsort(void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
    unsigned char* a = (unsigned char*)base;
    unsigned char* tmp = (unsigned char*)malloc(size);
    if (!tmp) {
        return;
    }
    for (size_t i = 0; i < nmemb; i++) {
        for (size_t j = i + 1; j < nmemb; j++) {
            unsigned char* pi = a + i * size;
            unsigned char* pj = a + j * size;
            if (compar(pi, pj) > 0) {
                memcpy(tmp, pi, size);
                memcpy(pi, pj, size);
                memcpy(pj, tmp, size);
            }
        }
    }
    free(tmp);
}

void* bsearch(const void* key, const void* base, size_t nmemb, size_t size, int (*compar)(const void*, const void*)) {
    const unsigned char* a = (const unsigned char*)base;
    size_t lo = 0;
    size_t hi = nmemb;
    while (lo < hi) {
        size_t mid = lo + (hi - lo) / 2;
        const void* p = a + mid * size;
        int cmp = compar(key, p);
        if (cmp < 0) {
            hi = mid;
        } else if (cmp > 0) {
            lo = mid + 1;
        } else {
            return (void*)p;
        }
    }
    return 0;
}

int atoi(const char* nptr) {
    return (int)strtol(nptr, 0, 10);
}

static int digit_value(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return -1;
}

long int strtol(const char* nptr, char** endptr, int base) {
    const char* s = nptr;
    long sign = 1;
    long value = 0;
    int d;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v') s++;
    if (*s == '+') s++;
    else if (*s == '-') { sign = -1; s++; }

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    }

    while ((d = digit_value(*s)) >= 0 && d < base) {
        value = value * base + d;
        s++;
    }

    if (endptr) *endptr = (char*)s;
    return value * sign;
}

unsigned long int strtoul(const char* nptr, char** endptr, int base) {
    const char* s = nptr;
    unsigned long value = 0;
    int d;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v') s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    }

    while ((d = digit_value(*s)) >= 0 && d < base) {
        value = value * base + (unsigned long)d;
        s++;
    }

    if (endptr) *endptr = (char*)s;
    return value;
}

long long int strtoll(const char* nptr, char** endptr, int base) {
    const char* s = nptr;
    long long sign = 1;
    unsigned long long value = 0;
    int d;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v') s++;
    if (*s == '+') s++;
    else if (*s == '-') { sign = -1; s++; }

    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    }

    while ((d = digit_value(*s)) >= 0 && d < base) {
        value = value * (unsigned long long)base + (unsigned long long)d;
        s++;
    }

    if (endptr) *endptr = (char*)s;
    return (long long)(value * (unsigned long long)sign);
}

unsigned long long int strtoull(const char* nptr, char** endptr, int base) {
    const char* s = nptr;
    unsigned long long value = 0;
    int d;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v') s++;
    if (base == 0) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
            base = 16;
            s += 2;
        } else if (s[0] == '0') {
            base = 8;
        } else {
            base = 10;
        }
    }

    while ((d = digit_value(*s)) >= 0 && d < base) {
        value = value * (unsigned long long)base + (unsigned long long)d;
        s++;
    }

    if (endptr) *endptr = (char*)s;
    return value;
}

float strtof(const char* nptr, char** endptr) {
    return (float)strtold(nptr, endptr);
}

long double strtold(const char* nptr, char** endptr) {
    const char* s = nptr;
    int sign = 1;
    long double value = 0.0;
    int d;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\f' || *s == '\v') s++;
    if (*s == '+') s++;
    else if (*s == '-') { sign = -1; s++; }

    while ((d = digit_value(*s)) >= 0 && d < 10) {
        value = value * 10.0 + (long double)d;
        s++;
    }

    if (*s == '.') {
        long double place = 0.1;
        s++;
        while ((d = digit_value(*s)) >= 0 && d < 10) {
            value += (long double)d * place;
            place *= 0.1;
            s++;
        }
    }

    if (endptr) *endptr = (char*)s;
    return value * sign;
}

double strtod(const char* nptr, char** endptr) {
    return (double)strtold(nptr, endptr);
}

long double ldexpl(long double x, int exp) {
    if (exp > 0) {
        while (exp--) x *= 2.0L;
    } else {
        while (exp++) x *= 0.5L;
    }
    return x;
}

void exit(int code) {
    sys_exit(code);
}

char* getenv(const char* name) {
    (void)name;
    return 0;
}

void* dlopen(const char* filename, int flag) {
    (void)filename;
    (void)flag;
    return 0;
}

const char* dlerror(void) {
    return "dlerror unsupported";
}

void* dlsym(void* handle, char* symbol) {
    (void)handle;
    (void)symbol;
    return 0;
}

int dlclose(void* handle) {
    (void)handle;
    return 0;
}
