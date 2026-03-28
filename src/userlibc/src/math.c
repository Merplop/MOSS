/*
 * MOSS user-space libc — minimal math functions.
 *
 * Only provides what doomgeneric needs for its lookup table initialisation.
 * All trig functions use double-precision Taylor/polynomial approximations
 * that are more than adequate for 16.16 fixed-point table generation.
 */
#include <math.h>

/* ---- helpers ---- */

static const double PI  = 3.14159265358979323846;
static const double PI2 = 6.28318530717958647692;

/* Reduce x into [-PI, PI) */
static double _reduce(double x) {
    x = fmod(x, PI2);
    if (x > PI)  x -= PI2;
    if (x < -PI) x += PI2;
    return x;
}

/* ---- basic ---- */

double fabs(double x) {
    return x < 0.0 ? -x : x;
}

double fmod(double x, double y) {
    if (y == 0.0) return 0.0;
    double q = x / y;
    /* truncate toward zero */
    long qi = (long)q;
    return x - (double)qi * y;
}

double floor(double x) {
    long i = (long)x;
    if (x < 0.0 && (double)i != x)
        return (double)(i - 1);
    return (double)i;
}

double ceil(double x) {
    long i = (long)x;
    if (x > 0.0 && (double)i != x)
        return (double)(i + 1);
    return (double)i;
}

double round(double x) {
    return floor(x + 0.5);
}

/* Newton's method sqrt */
double sqrt(double x) {
    if (x <= 0.0) return 0.0;
    double g = x * 0.5;
    for (int i = 0; i < 20; i++)
        g = 0.5 * (g + x / g);
    return g;
}

/* ---- trig (Taylor series) ---- */

double sin(double x) {
    x = _reduce(x);
    /* sin(x) = x - x^3/3! + x^5/5! - x^7/7! + ... */
    double term = x;
    double sum  = x;
    for (int n = 1; n <= 10; n++) {
        term *= -x * x / (double)((2 * n) * (2 * n + 1));
        sum += term;
    }
    return sum;
}

double cos(double x) {
    x = _reduce(x);
    double term = 1.0;
    double sum  = 1.0;
    for (int n = 1; n <= 10; n++) {
        term *= -x * x / (double)((2 * n - 1) * (2 * n));
        sum += term;
    }
    return sum;
}

double tan(double x) {
    double c = cos(x);
    if (c == 0.0) return 1e30;  /* avoid division by zero */
    return sin(x) / c;
}

/* ---- float wrappers ---- */

float sinf(float x) { return (float)sin((double)x); }
float cosf(float x) { return (float)cos((double)x); }
float tanf(float x) { return (float)tan((double)x); }
float fabsf(float x) { return x < 0.0f ? -x : x; }
float floorf(float x) { return (float)floor((double)x); }
float ceilf(float x) { return (float)ceil((double)x); }
float sqrtf(float x) { return (float)sqrt((double)x); }

/* ---- ldexp / frexp ---- */

double ldexp(double x, int exp) {
    /* x * 2^exp */
    double factor = 1.0;
    int e = exp < 0 ? -exp : exp;
    for (int i = 0; i < e; i++)
        factor *= 2.0;
    return exp < 0 ? x / factor : x * factor;
}

double frexp(double x, int *exp) {
    if (x == 0.0) {
        *exp = 0;
        return 0.0;
    }
    int e = 0;
    double ax = fabs(x);
    while (ax >= 1.0) { ax *= 0.5; e++; }
    while (ax < 0.5)  { ax *= 2.0; e--; }
    *exp = e;
    return x < 0.0 ? -ax : ax;
}

float ldexpf(float x, int exp) { return (float)ldexp((double)x, exp); }
float frexpf(float x, int *exp) { return (float)frexp((double)x, exp); }

/* ---- logarithms ---- */

double log(double x) {
    if (x <= 0.0) return -1e30;
    /* Use series: ln(x) = 2 * atanh((x-1)/(x+1)) */
    double y = (x - 1.0) / (x + 1.0);
    double y2 = y * y;
    double sum = y;
    double term = y;
    for (int n = 1; n <= 20; n++) {
        term *= y2;
        sum += term / (2.0 * n + 1.0);
    }
    return 2.0 * sum;
}

double log2(double x) {
    return log(x) / 0.6931471805599453;  /* ln(2) */
}

double log10(double x) {
    return log(x) / 2.302585092994046;   /* ln(10) */
}

/* ---- exp ---- */

double exp(double x) {
    /* Taylor series: e^x = 1 + x + x^2/2! + x^3/3! + ... */
    double sum = 1.0;
    double term = 1.0;
    for (int n = 1; n <= 30; n++) {
        term *= x / (double)n;
        sum += term;
    }
    return sum;
}

/* ---- pow ---- */

double pow(double base, double exponent) {
    if (exponent == 0.0) return 1.0;
    if (base == 0.0) return 0.0;
    /* Check for integer exponent */
    int iexp = (int)exponent;
    if ((double)iexp == exponent && iexp >= 0) {
        double result = 1.0;
        for (int i = 0; i < iexp; i++)
            result *= base;
        return result;
    }
    /* General case: base^exp = e^(exp * ln(base)) */
    if (base < 0.0) return 0.0;  /* undefined for non-integer exp */
    return exp(exponent * log(base));
}

/* ---- inverse trig ---- */

double atan(double x) {
    /* For |x| <= 1, use Taylor series.
     * For |x| > 1, use atan(x) = pi/2 - atan(1/x). */
    if (x > 1.0)  return  PI / 2.0 - atan(1.0 / x);
    if (x < -1.0) return -PI / 2.0 - atan(1.0 / x);
    double x2 = x * x;
    double sum = x;
    double term = x;
    for (int n = 1; n <= 25; n++) {
        term *= -x2;
        sum += term / (2.0 * n + 1.0);
    }
    return sum;
}

double atan2(double y, double x) {
    if (x > 0.0)       return atan(y / x);
    if (x < 0.0 && y >= 0.0) return atan(y / x) + PI;
    if (x < 0.0 && y < 0.0)  return atan(y / x) - PI;
    if (x == 0.0 && y > 0.0)  return  PI / 2.0;
    if (x == 0.0 && y < 0.0)  return -PI / 2.0;
    return 0.0;
}

double asin(double x) {
    if (x < -1.0 || x > 1.0) return 0.0;
    return atan2(x, sqrt(1.0 - x * x));
}

double acos(double x) {
    if (x < -1.0 || x > 1.0) return 0.0;
    return atan2(sqrt(1.0 - x * x), x);
}
