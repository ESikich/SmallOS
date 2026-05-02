#include "user_lib.h"
#include "time.h"

static const char* s_months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

struct tm* gmtime_r(const time_t* timep, struct tm* result) {
    time_t v = timep ? *timep : time(0);
    memset(result, 0, sizeof(*result));
    result->tm_year = 70 + (int)(v / 31536000u);
    result->tm_mon = 0;
    result->tm_mday = 1;
    result->tm_hour = (int)((v / 3600u) % 24u);
    result->tm_min = (int)((v / 60u) % 60u);
    result->tm_sec = (int)(v % 60u);
    return result;
}

static void append_ch(char* out, size_t max, size_t* pos, char c) {
    if (*pos + 1u < max) {
        out[*pos] = c;
    }
    (*pos)++;
}

static void append_str(char* out, size_t max, size_t* pos, const char* s) {
    while (*s) {
        append_ch(out, max, pos, *s++);
    }
}

static void append_u32(char* out, size_t max, size_t* pos, unsigned int v, unsigned int width) {
    char buf[16];
    unsigned int i = 0;
    do {
        buf[i++] = (char)('0' + (v % 10u));
        v /= 10u;
    } while (v > 0u && i < sizeof(buf));
    while (i < width && i < sizeof(buf)) {
        buf[i++] = '0';
    }
    while (i > 0u) {
        append_ch(out, max, pos, buf[--i]);
    }
}

size_t strftime(char* s, size_t max, const char* format, const struct tm* tm) {
    size_t pos = 0;
    if (!s || max == 0 || !format || !tm) return 0;

    for (const char* p = format; *p; p++) {
        if (*p != '%') {
            append_ch(s, max, &pos, *p);
            continue;
        }
        p++;
        switch (*p) {
        case '%':
            append_ch(s, max, &pos, '%');
            break;
        case 'b':
            append_str(s, max, &pos, s_months[(tm->tm_mon >= 0 && tm->tm_mon < 12) ? tm->tm_mon : 0]);
            break;
        case 'd':
            append_u32(s, max, &pos, (unsigned int)tm->tm_mday, 2);
            break;
        case 'H':
            append_u32(s, max, &pos, (unsigned int)tm->tm_hour, 2);
            break;
        case 'M':
            append_u32(s, max, &pos, (unsigned int)tm->tm_min, 2);
            break;
        case 'S':
            append_u32(s, max, &pos, (unsigned int)tm->tm_sec, 2);
            break;
        case 'Y':
            append_u32(s, max, &pos, (unsigned int)(tm->tm_year + 1900), 4);
            break;
        default:
            append_ch(s, max, &pos, *p);
            break;
        }
    }

    if (pos >= max) {
        if (max > 0) s[max - 1u] = '\0';
        return 0;
    }
    s[pos] = '\0';
    return pos;
}
