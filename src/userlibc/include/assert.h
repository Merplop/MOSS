/* <assert.h> — MOSS user libc. */
#ifndef _ASSERT_H
#define _ASSERT_H

#include <stdio.h>
#include <stdlib.h>

#ifdef NDEBUG
#define assert(expr) ((void)0)
#else
#define assert(expr) \
    do { \
        if (!(expr)) { \
            printf("assert failed: %s (%s:%d)\n", #expr, __FILE__, __LINE__); \
            abort(); \
        } \
    } while (0)
#endif

#endif /* _ASSERT_H */
