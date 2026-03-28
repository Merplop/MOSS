/* <sys/time.h> — MOSS user libc stub for TCC. */
#ifndef _SYS_TIME_H
#define _SYS_TIME_H

#include <time.h>

struct timeval {
    long tv_sec;
    long tv_usec;
};

struct timezone {
    int tz_minuteswest;
    int tz_dsttime;
};

static inline int gettimeofday(struct timeval *tv, struct timezone *tz) {
    (void)tz;
    if (tv) {
        time_t t = time(NULL);
        tv->tv_sec = t;
        tv->tv_usec = 0;
    }
    return 0;
}

#endif /* _SYS_TIME_H */
