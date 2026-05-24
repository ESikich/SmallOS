#include "stdlib.h"
#include "errno.h"
#include "sys/wait.h"
#include "unistd.h"

static char* format_long(long value, unsigned long uvalue,
                         int is_signed, char* str, int base) {
    static const char digits[] = "0123456789abcdefghijklmnopqrstuvwxyz";
    char tmp[34];
    unsigned long n;
    unsigned int pos = 0;
    unsigned int out = 0;

    if (!str || base < 2 || base > 36) {
        return str;
    }
    if (is_signed && value < 0 && base == 10) {
        str[out++] = '-';
        n = (unsigned long)(-value);
    } else {
        n = is_signed ? (unsigned long)value : uvalue;
    }
    do {
        tmp[pos++] = digits[n % (unsigned long)base];
        n /= (unsigned long)base;
    } while (n && pos < sizeof(tmp));
    while (pos) {
        str[out++] = tmp[--pos];
    }
    str[out] = '\0';
    return str;
}

__attribute__((weak)) char* ltoa(long value, char* str, int base) {
    return format_long(value, 0, 1, str, base);
}

__attribute__((weak)) char* ultoa(unsigned long value, char* str, int base) {
    return format_long(0, value, 0, str, base);
}

__attribute__((weak)) char* itoa(int value, char* str, int base) {
    return format_long((long)value, 0, 1, str, base);
}

int abs(int x) {
    return x < 0 ? -x : x;
}

static unsigned int s_rand_state = 1;

void srand(unsigned int seed) {
    s_rand_state = seed ? seed : 1u;
}

int rand(void) {
    s_rand_state = s_rand_state * 1103515245u + 12345u;
    return (int)((s_rand_state >> 16) & 0x7fff);
}

double atof(const char* nptr) {
    return strtod(nptr, NULL);
}

long atol(const char* nptr) {
    return strtol(nptr, NULL, 10);
}

int system(const char* command) {
    int status = 0;
    pid_t pid;

    if (!command) {
        return access("/bin/shell", X_OK) == 0 ? 1 : 0;
    }

    pid = fork();
    if (pid < 0) {
        return -1;
    }

    if (pid == 0) {
        char* const argv[] = { "shell", "-c", (char*)command, 0 };
        execv("/bin/shell", argv);
        exit(127);
    }

    for (;;) {
        pid_t waited = waitpid(pid, &status, 0);
        if (waited == pid) {
            return status;
        }
        if (waited < 0 && errno == EINTR) {
            continue;
        }
        return -1;
    }
}
