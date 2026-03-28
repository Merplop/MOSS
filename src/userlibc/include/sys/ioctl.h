/*
 * MOSS user-space libc — <sys/ioctl.h>
 * Provides ioctl() and TIOCGWINSZ for terminal size queries.
 */
#ifndef _SYS_IOCTL_H
#define _SYS_IOCTL_H

#include <stdint.h>
#include <syscall.h>

#define TIOCGWINSZ 0x5413

struct winsize {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
};

static inline int ioctl(int fd, unsigned long request, ...) {
    (void)fd;
    if (request == TIOCGWINSZ) {
        /* Use SYS_GET_WINSIZE to fill out the winsize struct */
        __builtin_va_list ap;
        __builtin_va_start(ap, request);
        struct winsize *ws = __builtin_va_arg(ap, struct winsize *);
        __builtin_va_end(ap);

        if (!ws) return -1;

        uint32_t dims[2]; /* [rows, cols] */
        int32_t ret;
        asm volatile("int $0x80" : "=a"(ret)
                     : "a"((uint32_t)SYS_GET_WINSIZE), "b"((uint32_t)dims)
                     : "memory");
        if (ret != 0) return -1;

        ws->ws_row = (uint16_t)dims[0];
        ws->ws_col = (uint16_t)dims[1];
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }
    return -1;
}

#endif /* _SYS_IOCTL_H */
