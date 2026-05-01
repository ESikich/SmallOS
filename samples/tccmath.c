static void sys_write(const char* s, int len) {
    __asm__ volatile(
        "int $0x80"
        :
        : "a"(1), "b"(s), "c"(len)
        : "memory"
    );
}

static void sys_exit(int code) {
    __asm__ volatile(
        "int $0x80"
        :
        : "a"(2), "b"(code)
        : "memory"
    );
}

void* memcpy(void* dst, const void* src, unsigned int len) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    unsigned int i = 0;
    while (i < len) {
        d[i] = s[i];
        i++;
    }
    return dst;
}

void* memmove(void* dst, const void* src, unsigned int len) {
    unsigned char* d = (unsigned char*)dst;
    const unsigned char* s = (const unsigned char*)src;
    unsigned int i;
    if (d == s || len == 0) {
        return dst;
    }
    if (d < s) {
        return memcpy(dst, src, len);
    }
    i = len;
    while (i > 0) {
        i--;
        d[i] = s[i];
    }
    return dst;
}

void* memset(void* dst, int value, unsigned int len) {
    unsigned char* d = (unsigned char*)dst;
    unsigned char c = (unsigned char)value;
    unsigned int i = 0;
    while (i < len) {
        d[i] = c;
        i++;
    }
    return dst;
}

static int add(int a, int b) {
    return a + b;
}

static int fib(int n) {
    int a = 0;
    int b = 1;
    while (n-- > 0) {
        int next = a + b;
        a = b;
        b = next;
    }
    return a;
}

static int checksum(const int* values, int len) {
    int sum = 0;
    int i = 0;
    while (i < len) {
        sum += values[i] * (i + 1);
        i++;
    }
    return sum;
}

static int factorial(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * factorial(n - 1);
}

static int bonus_pick(int mode) {
    switch (mode) {
        case 0: return 3;
        case 1: return 5;
        case 2: return 7;
        default: return 11;
    }
}

static char* append_str(char* out, const char* s) {
    while (*s) {
        *out++ = *s++;
    }
    return out;
}

static char* append_dec(char* out, int value) {
    char tmp[16];
    int pos = 0;
    unsigned int n;

    if (value < 0) {
        *out++ = '-';
        n = (unsigned int)(-value);
    } else {
        n = (unsigned int)value;
    }

    if (n == 0) {
        *out++ = '0';
        return out;
    }

    while (n > 0) {
        tmp[pos++] = (char)('0' + (n % 10u));
        n /= 10u;
    }

    while (pos > 0) {
        *out++ = tmp[--pos];
    }

    return out;
}

void _start(void) {
    int values[4];
    int scratch[6];
    int a = add(2, 5);
    int b = fib(6);
    int c;
    int f;
    int bonus;
    int total;
    char buf[128];
    char* p = buf;

    values[0] = 3;
    values[1] = 1;
    values[2] = 4;
    values[3] = 1;
    memset(scratch, 0, sizeof(scratch));
    memcpy(&scratch[0], values, sizeof(values));
    memmove(&scratch[1], &scratch[0], sizeof(values));
    c = checksum(values, 4);
    f = factorial(5);
    bonus = bonus_pick(2);
    total = a + b + c + scratch[3] + f + bonus;

    p = append_str(p, "tcc math ok: add=");
    p = append_dec(p, a);
    p = append_str(p, " fib=");
    p = append_dec(p, b);
    p = append_str(p, " checksum=");
    p = append_dec(p, c);
    p = append_str(p, " scratch=");
    p = append_dec(p, scratch[3]);
    p = append_str(p, " fact=");
    p = append_dec(p, f);
    p = append_str(p, " bonus=");
    p = append_dec(p, bonus);
    p = append_str(p, " total=");
    p = append_dec(p, total);
    p = append_str(p, "\n");

    sys_write(buf, (int)(p - buf));
    sys_exit(0);
}
