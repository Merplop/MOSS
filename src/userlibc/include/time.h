/* <time.h> — MOSS user libc (minimal).
 * Provides enough for TCC's __DATE__ / __TIME__ support. */
#ifndef _TIME_H
#define _TIME_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int32_t  time_t;
typedef uint32_t clock_t;

#define CLOCKS_PER_SEC 100   /* matches PIT tick rate */

struct tm {
    int tm_sec;    /* 0-59 */
    int tm_min;    /* 0-59 */
    int tm_hour;   /* 0-23 */
    int tm_mday;   /* 1-31 */
    int tm_mon;    /* 0-11 */
    int tm_year;   /* years since 1900 */
    int tm_wday;   /* 0-6 (Sunday=0) */
    int tm_yday;   /* 0-365 */
    int tm_isdst;  /* daylight saving */
};

/* Returns seconds since boot (MOSS has no RTC). */
time_t time(time_t *tloc);

/* Returns static struct tm set to a fixed date (MOSS has no RTC). */
struct tm *localtime(const time_t *timep);
struct tm *gmtime(const time_t *timep);

/* Returns tick count (CLOCKS_PER_SEC = 100). */
clock_t clock(void);

/* Format time string. */
size_t strftime(char *s, size_t max, const char *format, const struct tm *tm);

/* Difference in seconds. */
double difftime(time_t t1, time_t t0);

/* Convert tm to time_t. */
time_t mktime(struct tm *tm);

#ifdef __cplusplus
}
#endif

#endif /* _TIME_H */
