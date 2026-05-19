#ifndef FRACTINT_SMALLOS_MATH_H
#define FRACTINT_SMALLOS_MATH_H

#include_next <math.h>

#ifndef HUGE_VAL
#define HUGE_VAL (1.0e300)
#endif

double fabs(double x);
double floor(double x);
double ceil(double x);
double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double sinh(double x);
double cosh(double x);
double atan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);
double log(double x);
double log10(double x);
double exp(double x);
double pow(double x, double y);
double fmod(double x, double y);
double frexp(double x, int* exp);
int isnan(double x);
int isinf(double x);
int islessequal(double x, double y);
long double sinhl(long double x);
long double coshl(long double x);

#endif
