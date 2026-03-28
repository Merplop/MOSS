/*
 * MOSS user-space libc — <math.h>
 * Minimal math functions needed by doomgeneric (table init).
 */
#ifndef _MATH_H
#define _MATH_H

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

double sin(double x);
double cos(double x);
double tan(double x);
double fabs(double x);
double floor(double x);
double ceil(double x);
double sqrt(double x);
double round(double x);
double fmod(double x, double y);
double ldexp(double x, int exp);
double frexp(double x, int *exp);
double log(double x);
double log2(double x);
double log10(double x);
double exp(double x);
double pow(double base, double exponent);
double atan(double x);
double atan2(double y, double x);
double asin(double x);
double acos(double x);

float sinf(float x);
float cosf(float x);
float tanf(float x);
float fabsf(float x);
float floorf(float x);
float ceilf(float x);
float sqrtf(float x);
float ldexpf(float x, int exp);
float frexpf(float x, int *exp);

#define HUGE_VAL  (__builtin_huge_val())
#define INFINITY  (__builtin_inff())
#define NAN       (__builtin_nanf(""))
#define isnan(x)  (__builtin_isnan(x))
#define isinf(x)  (__builtin_isinf(x))
#define isfinite(x) (__builtin_isfinite(x))

#endif /* _MATH_H */
