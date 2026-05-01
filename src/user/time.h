#ifndef USER_TIME_WRAPPER_H
#define USER_TIME_WRAPPER_H

typedef unsigned int time_t;

struct timespec {
    time_t tv_sec;
    long tv_nsec;
};

struct tm {
    int tm_sec;
    int tm_min;
    int tm_hour;
    int tm_mday;
    int tm_mon;
    int tm_year;
    int tm_wday;
    int tm_yday;
    int tm_isdst;
};

time_t time(time_t* out);
struct tm* localtime(const time_t* timep);
struct tm* gmtime_r(const time_t* timep, struct tm* result);
size_t strftime(char* s, size_t max, const char* format, const struct tm* tm);
int clock_gettime(int clock_id, struct timespec* ts);

#define CLOCK_REALTIME 0

#endif
