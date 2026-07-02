#include "stdint.h"

typedef unsigned long long u64;
typedef long long s64;

static u64 udivmod64(u64 num, u64 den, u64* rem) {
    u64 quot = 0;
    u64 bit = 1;

    if (den == 0) {
        if (rem) *rem = 0;
        return 0;
    }

    while ((den & (1ull << 63)) == 0 && den < num) {
        den <<= 1;
        bit <<= 1;
    }

    while (bit) {
        if (num >= den) {
            num -= den;
            quot |= bit;
        }
        den >>= 1;
        bit >>= 1;
    }

    if (rem) *rem = num;
    return quot;
}

u64 __udivdi3(u64 num, u64 den) {
    return udivmod64(num, den, 0);
}

u64 __umoddi3(u64 num, u64 den) {
    u64 rem = 0;
    (void)udivmod64(num, den, &rem);
    return rem;
}

s64 __divdi3(s64 num, s64 den) {
    int neg = 0;
    u64 unum;
    u64 uden;
    u64 q;

    if (num < 0) {
        neg = !neg;
        unum = (u64)(-num);
    } else {
        unum = (u64)num;
    }
    if (den < 0) {
        neg = !neg;
        uden = (u64)(-den);
    } else {
        uden = (u64)den;
    }

    q = udivmod64(unum, uden, 0);
    return neg ? -(s64)q : (s64)q;
}

s64 __moddi3(s64 num, s64 den) {
    int neg = 0;
    u64 rem = 0;
    u64 unum;
    u64 uden;

    if (num < 0) {
        neg = 1;
        unum = (u64)(-num);
    } else {
        unum = (u64)num;
    }
    uden = den < 0 ? (u64)(-den) : (u64)den;

    (void)udivmod64(unum, uden, &rem);
    return neg ? -(s64)rem : (s64)rem;
}
