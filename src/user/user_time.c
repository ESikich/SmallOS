#include "user_lib.h"
#include "time.h"

static const char* s_months[] = {
    "Jan", "Feb", "Mar", "Apr", "May", "Jun",
    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
};

static const char* s_weekdays[] = {
    "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
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

static int str_eq_n(const char* a, const char* b, unsigned int n) {
    for (unsigned int i = 0; i < n; i++) {
        if (a[i] != b[i]) return 0;
    }
    return 1;
}

static int str_eq(const char* a, const char* b) {
    while (*a && *b && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

static int parse_u2(const char** p, int* out) {
    int value = 0;
    int digits = 0;
    while (**p >= '0' && **p <= '9' && digits < 2) {
        value = value * 10 + (**p - '0');
        (*p)++;
        digits++;
    }
    if (digits == 0) return 0;
    *out = value;
    return 1;
}

static int parse_u4(const char** p, int* out) {
    int value = 0;
    for (int i = 0; i < 4; i++) {
        if ((*p)[i] < '0' || (*p)[i] > '9') return 0;
        value = value * 10 + ((*p)[i] - '0');
    }
    *p += 4;
    *out = value;
    return 1;
}

static int parse_name(const char** p, const char* const* names, int count) {
    for (int i = 0; i < count; i++) {
        if (str_eq_n(*p, names[i], 3)) {
            *p += 3;
            return i;
        }
    }
    return -1;
}

char* strptime(const char* buf, const char* fmt, struct tm* tm) {
    const char* p = buf;

    if (!buf || !fmt || !tm) return 0;

    if (!str_eq(fmt, "%a, %d %b %Y %H:%M:%S GMT")) {
        return 0;
    }

    int wday = parse_name(&p, s_weekdays, 7);
    if (wday < 0 || *p++ != ',' || *p++ != ' ') return 0;
    tm->tm_wday = wday;
    if (!parse_u2(&p, &tm->tm_mday) || *p++ != ' ') return 0;
    int mon = parse_name(&p, s_months, 12);
    if (mon < 0 || *p++ != ' ') return 0;
    tm->tm_mon = mon;
    int year = 0;
    if (!parse_u4(&p, &year) || *p++ != ' ') return 0;
    tm->tm_year = year - 1900;
    if (!parse_u2(&p, &tm->tm_hour) || *p++ != ':') return 0;
    if (!parse_u2(&p, &tm->tm_min) || *p++ != ':') return 0;
    if (!parse_u2(&p, &tm->tm_sec) || *p++ != ' ') return 0;
    if (!str_eq_n(p, "GMT", 3)) return 0;
    return (char*)p + 3;
}

static int is_leap(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

time_t timegm(struct tm* tm) {
    static const int month_days[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    unsigned int days = 0;
    int year;

    if (!tm) return 0;
    year = tm->tm_year + 1900;
    if (year < 1970) return 0;

    for (int y = 1970; y < year; y++) {
        days += is_leap(y) ? 366u : 365u;
    }
    for (int m = 0; m < tm->tm_mon && m < 12; m++) {
        days += (unsigned int)month_days[m];
        if (m == 1 && is_leap(year)) days++;
    }
    if (tm->tm_mday > 0) {
        days += (unsigned int)(tm->tm_mday - 1);
    }

    return (time_t)(days * 86400u +
                    (unsigned int)tm->tm_hour * 3600u +
                    (unsigned int)tm->tm_min * 60u +
                    (unsigned int)tm->tm_sec);
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
