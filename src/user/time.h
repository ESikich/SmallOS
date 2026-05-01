#ifndef USER_TIME_WRAPPER_H
#define USER_TIME_WRAPPER_H

typedef unsigned int time_t;

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

#endif
