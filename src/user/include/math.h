#ifndef USER_MATH_WRAPPER_H
#define USER_MATH_WRAPPER_H

float strtof(const char* nptr, char** endptr);
long double strtold(const char* nptr, char** endptr);

#ifndef HUGE_VAL
#define HUGE_VAL (1.0e300)
#endif

double fabs(double x);
double floor(double x);
double ceil(double x);
double fmod(double x, double y);
double sqrt(double x);
double sin(double x);
double cos(double x);
double tan(double x);
double atan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);
double exp(double x);
double ldexp(double x, int exp);
double frexp(double x, int* exp_out);
double log(double x);
double log10(double x);
double pow(double x, double y);
double sinh(double x);
double cosh(double x);
long double sinhl(long double x);
long double coshl(long double x);
int isnan(double x);
int isinf(double x);
int islessequal(double x, double y);

#endif
