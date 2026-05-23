#include "math.h"

static const double pi = 3.14159265358979323846;
static const double half_pi = 1.57079632679489661923;
static const double two_pi = 6.28318530717958647692;

double fabs(double x) {
    return x < 0.0 ? -x : x;
}

double floor(double x) {
    long i = (long)x;
    if ((double)i > x) {
        i--;
    }
    return (double)i;
}

double ceil(double x) {
    long i = (long)x;
    if ((double)i < x) {
        i++;
    }
    return (double)i;
}

double fmod(double x, double y) {
    long q;
    if (y == 0.0) {
        return 0.0;
    }
    q = (long)(x / y);
    return x - (double)q * y;
}

double sqrt(double x) {
    double g;
    int i;
    if (x <= 0.0) {
        return 0.0;
    }
    g = x > 1.0 ? x : 1.0;
    for (i = 0; i < 24; i++) {
        g = 0.5 * (g + x / g);
    }
    return g;
}

static double reduce_angle(double x) {
    x = fmod(x, two_pi);
    if (x > pi) x -= two_pi;
    if (x < -pi) x += two_pi;
    return x;
}

static double sin_kernel(double x) {
    double x2 = x * x;
    double term = x;
    double sum = x;

    term *= -x2 / (2.0 * 3.0);
    sum += term;
    term *= -x2 / (4.0 * 5.0);
    sum += term;
    term *= -x2 / (6.0 * 7.0);
    sum += term;
    term *= -x2 / (8.0 * 9.0);
    sum += term;
    term *= -x2 / (10.0 * 11.0);
    sum += term;
    term *= -x2 / (12.0 * 13.0);
    sum += term;
    term *= -x2 / (14.0 * 15.0);
    sum += term;
    term *= -x2 / (16.0 * 17.0);
    sum += term;
    return sum;
}

static double cos_kernel(double x) {
    double x2 = x * x;
    double term = 1.0;
    double sum = 1.0;

    term *= -x2 / (1.0 * 2.0);
    sum += term;
    term *= -x2 / (3.0 * 4.0);
    sum += term;
    term *= -x2 / (5.0 * 6.0);
    sum += term;
    term *= -x2 / (7.0 * 8.0);
    sum += term;
    term *= -x2 / (9.0 * 10.0);
    sum += term;
    term *= -x2 / (11.0 * 12.0);
    sum += term;
    term *= -x2 / (13.0 * 14.0);
    sum += term;
    term *= -x2 / (15.0 * 16.0);
    sum += term;
    return sum;
}

double sin(double x) {
    x = reduce_angle(x);
    if (x > half_pi) {
        x = pi - x;
    } else if (x < -half_pi) {
        x = -pi - x;
    }
    return sin_kernel(x);
}

double cos(double x) {
    int sign = 1;

    x = reduce_angle(x);
    if (x < 0.0) {
        x = -x;
    }
    if (x > half_pi) {
        x = pi - x;
        sign = -1;
    }
    return sign < 0 ? -cos_kernel(x) : cos_kernel(x);
}

double tan(double x) {
    double c = cos(x);
    if (fabs(c) < 1.0e-12) {
        return x < 0.0 ? -HUGE_VAL : HUGE_VAL;
    }
    return sin(x) / c;
}

double atan(double x) {
    int neg = x < 0.0;
    double r;
    if (neg) x = -x;
    if (x > 1.0) {
        r = 1.57079632679489661923 - atan(1.0 / x);
    } else {
        r = x / (1.0 + 0.28 * x * x);
    }
    return neg ? -r : r;
}

double atan2(double y, double x) {
    if (x > 0.0) return atan(y / x);
    if (x < 0.0 && y >= 0.0) return atan(y / x) + 3.14159265358979323846;
    if (x < 0.0 && y < 0.0) return atan(y / x) - 3.14159265358979323846;
    if (y > 0.0) return 1.57079632679489661923;
    if (y < 0.0) return -1.57079632679489661923;
    return 0.0;
}

double asin(double x) {
    if (x >= 1.0) return 1.57079632679489661923;
    if (x <= -1.0) return -1.57079632679489661923;
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    return 1.57079632679489661923 - asin(x);
}

double exp(double x) {
    const double ln2 = 0.69314718055994530942;
    int n = 0;
    double term;
    double sum;
    int i;

    if (x > 60.0) return HUGE_VAL;
    if (x < -60.0) return 0.0;
    while (x > ln2) { x -= ln2; n++; }
    while (x < -ln2) { x += ln2; n--; }
    term = 1.0;
    sum = 1.0;
    for (i = 1; i <= 18; i++) {
        term *= x / (double)i;
        sum += term;
    }
    while (n > 0) { sum *= 2.0; n--; }
    while (n < 0) { sum *= 0.5; n++; }
    return sum;
}

double frexp(double x, int* exp_out) {
    int e = 0;
    double ax;
    if (x == 0.0) {
        *exp_out = 0;
        return 0.0;
    }
    ax = fabs(x);
    while (ax >= 1.0) { ax *= 0.5; e++; }
    while (ax < 0.5) { ax *= 2.0; e--; }
    *exp_out = e;
    return x < 0.0 ? -ax : ax;
}

double log(double x) {
    int e;
    double m;
    double z;
    double z2;
    double term;
    double sum;
    int i;

    if (x <= 0.0) {
        return -HUGE_VAL;
    }
    m = frexp(x, &e) * 2.0;
    e--;
    z = (m - 1.0) / (m + 1.0);
    z2 = z * z;
    term = z;
    sum = 0.0;
    for (i = 1; i < 30; i += 2) {
        sum += term / (double)i;
        term *= z2;
    }
    return 2.0 * sum + (double)e * 0.69314718055994530942;
}

double log10(double x) {
    return log(x) / 2.30258509299404568402;
}

double pow(double x, double y) {
    if (x <= 0.0) {
        return 0.0;
    }
    return exp(y * log(x));
}

double sinh(double x) {
    double e = exp(x);
    double ie = exp(-x);
    return 0.5 * (e - ie);
}

double cosh(double x) {
    double e = exp(x);
    double ie = exp(-x);
    return 0.5 * (e + ie);
}

long double sinhl(long double x) {
    return (long double)sinh((double)x);
}

long double coshl(long double x) {
    return (long double)cosh((double)x);
}

int isnan(double x) {
    return x != x;
}

int isinf(double x) {
    return !isnan(x) && isnan(x - x);
}

int islessequal(double x, double y) {
    return x <= y;
}
