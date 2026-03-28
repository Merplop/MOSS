/*
 * MOSS user-space libc — <termios.h>
 * Stub for terminal control. MOSS has no terminal mode switching;
 * the keyboard driver is always in "raw" mode.
 * These exist so programs that include termios.h can compile.
 */
#ifndef _TERMIOS_H
#define _TERMIOS_H

#include <stdint.h>

typedef uint32_t tcflag_t;
typedef uint8_t  cc_t;
typedef uint32_t speed_t;

#define NCCS 20

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t     c_cc[NCCS];
};

/* c_lflag bits */
#define ECHO    0x0001
#define ICANON  0x0002
#define IEXTEN  0x0004
#define ISIG    0x0008

/* c_iflag bits */
#define IXON    0x0001
#define ICRNL   0x0002
#define BRKINT  0x0004
#define INPCK   0x0008
#define ISTRIP  0x0010

/* c_oflag bits */
#define OPOST   0x0001

/* c_cflag bits */
#define CS8     0x0030

/* c_cc indices */
#define VMIN    16
#define VTIME   17

/* tcsetattr actions */
#define TCSAFLUSH 2

/*
 * These are no-ops on MOSS — the keyboard is always raw,
 * and there is no line discipline to configure.
 */
static inline int tcgetattr(int fd, struct termios *t) {
    (void)fd;
    if (t) {
        t->c_iflag = 0;
        t->c_oflag = 0;
        t->c_cflag = CS8;
        t->c_lflag = 0;
        for (int i = 0; i < NCCS; i++) t->c_cc[i] = 0;
    }
    return 0;
}

static inline int tcsetattr(int fd, int action, const struct termios *t) {
    (void)fd; (void)action; (void)t;
    return 0;
}

#endif /* _TERMIOS_H */
