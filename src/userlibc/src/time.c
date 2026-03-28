/*
 * MOSS user-space libc — minimal time functions.
 * MOSS has no RTC, so time() returns seconds since boot.
 */
#include <time.h>
#include <string.h>
#include <stdio.h>
#include <syscall.h>

time_t time(time_t *tloc) {
    time_t t = (time_t)_syscall0(SYS_GETTIME);
    if (tloc)
        *tloc = t;
    return t;
}

clock_t clock(void) {
    return (clock_t)_syscall0(SYS_GETTICKS);
}

/* Static tm struct — MOSS has no RTC, so return a fixed date. */
static struct tm _static_tm;

struct tm *localtime(const time_t *timep) {
    time_t t = timep ? *timep : 0;
    memset(&_static_tm, 0, sizeof(_static_tm));
    /* Return a plausible fixed date: 2026-01-01 00:00:00 + seconds */
    _static_tm.tm_year = 126;  /* 2026 - 1900 */
    _static_tm.tm_mon  = 0;    /* January */
    _static_tm.tm_mday = 1;
    _static_tm.tm_hour = (int)((t / 3600) % 24);
    _static_tm.tm_min  = (int)((t / 60) % 60);
    _static_tm.tm_sec  = (int)(t % 60);
    _static_tm.tm_wday = 4;    /* Thursday */
    _static_tm.tm_yday = 0;
    return &_static_tm;
}

struct tm *gmtime(const time_t *timep) {
    return localtime(timep);
}

double difftime(time_t t1, time_t t0) {
    return (double)(t1 - t0);
}

time_t mktime(struct tm *tm) {
    if (!tm) return (time_t)-1;
    return (time_t)(tm->tm_hour * 3600 + tm->tm_min * 60 + tm->tm_sec);
}

size_t strftime(char *s, size_t max, const char *format, const struct tm *tm) {
    if (!s || max == 0 || !format || !tm)
        return 0;

    size_t pos = 0;
    while (*format && pos < max - 1) {
        if (*format != '%') {
            s[pos++] = *format++;
            continue;
        }
        format++;
        char tmp[32];
        const char *insert = tmp;
        int len = 0;

        switch (*format) {
        case 'Y':  /* 4-digit year */
            len = snprintf(tmp, sizeof(tmp), "%04d", tm->tm_year + 1900);
            break;
        case 'm':  /* month 01-12 */
            len = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mon + 1);
            break;
        case 'd':  /* day 01-31 */
            len = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_mday);
            break;
        case 'H':  /* hour 00-23 */
            len = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_hour);
            break;
        case 'M':  /* minute 00-59 */
            len = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_min);
            break;
        case 'S':  /* second 00-59 */
            len = snprintf(tmp, sizeof(tmp), "%02d", tm->tm_sec);
            break;
        case '%':
            tmp[0] = '%'; len = 1;
            break;
        default:
            tmp[0] = '%'; tmp[1] = *format; len = 2;
            break;
        }

        for (int i = 0; i < len && pos < max - 1; i++)
            s[pos++] = insert[i];
        format++;
    }
    s[pos] = '\0';
    return pos;
}
