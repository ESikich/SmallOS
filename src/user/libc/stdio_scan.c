#include <stdarg.h>

#include "stdio.h"
#include "stdlib.h"

static int scan_isspace(int c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' ||
           c == '\f' || c == '\v';
}

static int scan_token(FILE* stream, char* out, size_t cap) {
    int c;
    size_t n = 0;

    do {
        c = fgetc(stream);
        if (c == EOF) {
            return 0;
        }
    } while (scan_isspace(c));
    while (c != EOF && !scan_isspace(c)) {
        if (n + 1 < cap) {
            out[n++] = (char)c;
        }
        c = fgetc(stream);
    }
    out[n] = '\0';
    return 1;
}

static int char_in_set(int c, const char* set) {
    while (*set) {
        if (c == (unsigned char)*set++) {
            return 1;
        }
    }
    return 0;
}

int vsscanf(const char* input, const char* fmt, va_list ap) {
    int assigned = 0;

    while (*fmt) {
        if (scan_isspace((unsigned char)*fmt)) {
            while (scan_isspace((unsigned char)*fmt)) fmt++;
            while (scan_isspace((unsigned char)*input)) input++;
            continue;
        }
        if (*fmt != '%') {
            if (*input != *fmt) break;
            input++;
            fmt++;
            continue;
        }

        fmt++;
        int width = 0;
        int longmod = 0;
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == 'l') {
            longmod = 1;
            fmt++;
        }

        if (*fmt == 'c') {
            char* out = va_arg(ap, char*);
            int count = width ? width : 1;
            while (count-- && *input) {
                *out++ = *input++;
            }
            assigned++;
        } else if (*fmt == 'd' || *fmt == 'u' || *fmt == 'x') {
            char* end = NULL;
            int base = *fmt == 'x' ? 16 : 10;
            while (scan_isspace((unsigned char)*input)) input++;
            if (*fmt == 'u') {
                unsigned long value = strtoul(input, &end, base);
                unsigned int* out = va_arg(ap, unsigned int*);
                if (end == input) break;
                *out = (unsigned int)value;
            } else {
                long value = strtol(input, &end, base);
                if (longmod) {
                    long* out = va_arg(ap, long*);
                    *out = value;
                } else {
                    int* out = va_arg(ap, int*);
                    *out = (int)value;
                }
                if (end == input) break;
            }
            input = end;
            assigned++;
        } else if (*fmt == 'f') {
            char* end = NULL;
            double value;
            while (scan_isspace((unsigned char)*input)) input++;
            value = strtod(input, &end);
            if (end == input) break;
            if (longmod) {
                double* out = va_arg(ap, double*);
                *out = value;
            } else {
                float* out = va_arg(ap, float*);
                *out = (float)value;
            }
            input = end;
            assigned++;
        } else if (*fmt == 's') {
            char* out = va_arg(ap, char*);
            int count = 0;
            while (scan_isspace((unsigned char)*input)) input++;
            while (*input && !scan_isspace((unsigned char)*input) &&
                   (!width || count < width)) {
                *out++ = *input++;
                count++;
            }
            *out = '\0';
            if (count == 0) break;
            assigned++;
        } else if (*fmt == '[') {
            char* out = va_arg(ap, char*);
            char set[64];
            int neg = 0;
            int si = 0;
            int count = 0;
            fmt++;
            if (*fmt == '^') {
                neg = 1;
                fmt++;
            }
            while (*fmt && *fmt != ']' && si + 1 < (int)sizeof(set)) {
                set[si++] = *fmt++;
            }
            set[si] = '\0';
            while (*input && (!width || count < width)) {
                int in = char_in_set((unsigned char)*input, set);
                if ((in && neg) || (!in && !neg)) break;
                *out++ = *input++;
                count++;
            }
            *out = '\0';
            if (count == 0) break;
            assigned++;
        }

        if (*fmt) {
            fmt++;
        }
    }
    return assigned;
}

int sscanf(const char* input, const char* fmt, ...) {
    va_list ap;
    int rc;
    va_start(ap, fmt);
    rc = vsscanf(input, fmt, ap);
    va_end(ap);
    return rc;
}

int fscanf(FILE* stream, const char* fmt, ...) {
    va_list ap;
    int assigned = 0;
    int matched = 0;
    int input_failed = 0;

    va_start(ap, fmt);
    while (*fmt) {
        int suppress = 0;
        int width = 0;
        int longmod = 0;
        char tok[128];
        int c;
        int count;

        if (scan_isspace((unsigned char)*fmt)) {
            while (scan_isspace((unsigned char)*fmt)) fmt++;
            do {
                c = fgetc(stream);
            } while (c != EOF && scan_isspace(c));
            if (c != EOF) ungetc(c, stream);
            continue;
        }
        if (*fmt != '%') {
            c = fgetc(stream);
            if (c == EOF) {
                input_failed = 1;
                break;
            }
            if (c != (unsigned char)*fmt) {
                ungetc(c, stream);
                break;
            }
            fmt++;
            continue;
        }

        fmt++;
        if (*fmt == '*') {
            suppress = 1;
            fmt++;
        }
        while (*fmt >= '0' && *fmt <= '9') {
            width = width * 10 + (*fmt++ - '0');
        }
        if (*fmt == 'l') {
            longmod = 1;
            fmt++;
        }

        if (*fmt == 'd' || *fmt == 'u' || *fmt == 'x' || *fmt == 'X') {
            char* end = 0;
            int base = (*fmt == 'x' || *fmt == 'X') ? 16 : 10;
            count = 0;
            do {
                c = fgetc(stream);
            } while (c != EOF && scan_isspace(c));
            if (c == EOF) {
                input_failed = 1;
                break;
            }
            while (c != EOF && !scan_isspace(c) &&
                   (!width || count < width) &&
                   count + 1 < (int)sizeof(tok)) {
                tok[count++] = (char)c;
                c = fgetc(stream);
            }
            if (c != EOF) ungetc(c, stream);
            tok[count] = '\0';
            if (count == 0) break;
            if (*fmt == 'u' || *fmt == 'x' || *fmt == 'X') {
                unsigned long value = strtoul(tok, &end, base);
                if (end == tok) break;
                if (!suppress) {
                    if (longmod) {
                        unsigned long* out = va_arg(ap, unsigned long*);
                        *out = value;
                    } else {
                        unsigned int* out = va_arg(ap, unsigned int*);
                        *out = (unsigned int)value;
                    }
                    assigned++;
                }
            } else {
                long value = strtol(tok, &end, base);
                if (end == tok) break;
                if (!suppress) {
                    if (longmod) {
                        long* out = va_arg(ap, long*);
                        *out = value;
                    } else {
                        int* out = va_arg(ap, int*);
                        *out = (int)value;
                    }
                    assigned++;
                }
            }
            matched = 1;
        } else if (*fmt == 'f') {
            char* end = 0;
            count = 0;
            if (!scan_token(stream, tok, sizeof(tok))) {
                input_failed = 1;
                break;
            }
            if (!suppress) {
                double value = strtod(tok, &end);
                if (end == tok) break;
                if (longmod) {
                    double* out = va_arg(ap, double*);
                    *out = value;
                } else {
                    float* out = va_arg(ap, float*);
                    *out = (float)value;
                }
                assigned++;
            }
            matched = 1;
        } else if (*fmt == 's') {
            char* out = suppress ? 0 : va_arg(ap, char*);
            count = 0;
            do {
                c = fgetc(stream);
            } while (c != EOF && scan_isspace(c));
            if (c == EOF) {
                input_failed = 1;
                break;
            }
            while (c != EOF && !scan_isspace(c) && (!width || count < width)) {
                if (!suppress) out[count] = (char)c;
                count++;
                c = fgetc(stream);
            }
            if (c != EOF) ungetc(c, stream);
            if (count == 0) break;
            if (!suppress) {
                out[count] = '\0';
                assigned++;
            }
            matched = 1;
        } else if (*fmt == '[') {
            char set[64];
            int neg = 0;
            int si = 0;
            char* out = suppress ? 0 : va_arg(ap, char*);
            fmt++;
            if (*fmt == '^') {
                neg = 1;
                fmt++;
            }
            while (*fmt && *fmt != ']' && si + 1 < (int)sizeof(set)) {
                set[si++] = *fmt++;
            }
            set[si] = '\0';
            if (*fmt != ']') break;
            count = 0;
            c = fgetc(stream);
            while (c != EOF && (!width || count < width)) {
                int in = char_in_set(c, set);
                if ((in && neg) || (!in && !neg)) break;
                if (!suppress) out[count] = (char)c;
                count++;
                c = fgetc(stream);
            }
            if (c != EOF) ungetc(c, stream);
            if (count == 0) break;
            if (!suppress) {
                out[count] = '\0';
                assigned++;
            }
            matched = 1;
        }
        if (*fmt) {
            fmt++;
        }
    }
    va_end(ap);
    if (assigned) return assigned;
    if (matched) return 0;
    return input_failed ? EOF : 0;
}
