#include "stdio.h"
#include "string.h"
#include "time.h"
#include "errno.h"
#include "user_syscall.h"
#include "ntp.h"

static int parse_ipv4_host(const char* text, unsigned int* out) {
    unsigned int parts[4];
    unsigned int part = 0;
    unsigned int value = 0;
    int saw_digit = 0;

    if (!text || !out) return 0;
    for (const char* p = text; ; p++) {
        char c = *p;
        if (c >= '0' && c <= '9') {
            value = value * 10u + (unsigned int)(c - '0');
            if (value > 255u) return 0;
            saw_digit = 1;
            continue;
        }
        if (c == '.' || c == '\0') {
            if (!saw_digit || part >= 4u) return 0;
            parts[part++] = value;
            value = 0;
            saw_digit = 0;
            if (c == '\0') break;
            continue;
        }
        return 0;
    }
    if (part != 4u) return 0;
    *out = (parts[0] << 24) | (parts[1] << 16) | (parts[2] << 8) | parts[3];
    return 1;
}

static void print_date(time_t now) {
    struct tm tm;
    char buf[64];

    gmtime_r(&now, &tm);
    if (strftime(buf, sizeof(buf), "%a %b %d %H:%M:%S %Y UTC", &tm) == 0) {
        printf("%u\n", (unsigned int)now);
        return;
    }
    printf("%s\n", buf);
}

static void usage(void) {
    printf("usage: date [-s [server-ip]]\n");
}

void _start(int argc, char** argv) {
    struct timespec ts;
    unsigned int server_ip = NTP_DEFAULT_SERVER_IP;
    int rc;

    if (argc > 1) {
        if (strcmp(argv[1], "-s") != 0 && strcmp(argv[1], "--sync") != 0) {
            usage();
            sys_exit(1);
        }
        if (argc > 2 && !parse_ipv4_host(argv[2], &server_ip)) {
            printf("date: invalid NTP server IP: %s\n", argv[2]);
            sys_exit(1);
        }
        rc = sys_ntp_sync(server_ip, &ts);
        if (rc < 0) {
            printf("date: NTP sync failed (errno %d)\n", -rc);
            sys_exit(1);
        }
        print_date(ts.tv_sec);
        sys_exit(0);
    }

    if (clock_gettime(CLOCK_REALTIME, &ts) < 0) {
        printf("date: clock_gettime failed (errno %d)\n", errno);
        sys_exit(1);
    }
    print_date(ts.tv_sec);
    sys_exit(0);
}
