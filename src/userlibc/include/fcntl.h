/* <fcntl.h> — file control options (aliases from syscall.h) */
#ifndef _FCNTL_H
#define _FCNTL_H

#include <sys/types.h>
#include <syscall.h>

#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0040
#define O_TRUNC   0x0200
#define O_APPEND  0x0400
#define O_EXCL    0x0080
#define O_NONBLOCK 0x0800
#define O_CLOEXEC  0x80000

/* fcntl() commands */
#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define FD_CLOEXEC 1

int open(const char *path, int flags, ...);

static inline int fcntl(int fd, int cmd, ...) {
    /* We only support up to one extra arg */
    unsigned int arg = 0;
    /* Extract variadic arg via builtin */
    __builtin_va_list ap;
    __builtin_va_start(ap, cmd);
    arg = __builtin_va_arg(ap, unsigned int);
    __builtin_va_end(ap);
    return (int)_syscall3(SYS_FCNTL, (uint32_t)fd, (uint32_t)cmd, (uint32_t)arg);
}

#endif /* _FCNTL_H */
