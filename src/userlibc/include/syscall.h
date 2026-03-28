/*
 * MOSS user-space syscall interface.
 * Inline wrappers around int $0x80.
 */
#ifndef _MOSS_SYSCALL_H
#define _MOSS_SYSCALL_H

#include <stdint.h>

/* Syscall numbers (must match kernel/include/kernel/syscall.h) */
#define SYS_EXIT     0
#define SYS_WRITE    1
#define SYS_READ     2
#define SYS_GETPID   3
#define SYS_BRK      4
#define SYS_OPEN     5
#define SYS_CLOSE    6
#define SYS_LSEEK    7
#define SYS_FBMAP    8
#define SYS_GETTICKS 9
#define SYS_USLEEP   10
#define SYS_WRITE_N  11
#define SYS_POLL_KEY 12
#define SYS_WAIT_KEY 13
#define SYS_GET_WINSIZE 14
#define SYS_STAT        15
#define SYS_FSTAT       16
#define SYS_UNLINK      17
#define SYS_GETCWD      18
#define SYS_CHDIR       19
#define SYS_GETTIME     20
#define SYS_DUP         21
#define SYS_DUP2        22

/* open() flags */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0040
#define O_TRUNC   0x0200

/* lseek() whence */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

static inline int32_t _syscall0(uint32_t num) {
    int32_t ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline int32_t _syscall1(uint32_t num, uint32_t a1) {
    int32_t ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1) : "memory");
    return ret;
}

static inline int32_t _syscall2(uint32_t num, uint32_t a1, uint32_t a2) {
    int32_t ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2) : "memory");
    return ret;
}

static inline int32_t _syscall3(uint32_t num, uint32_t a1, uint32_t a2, uint32_t a3) {
    int32_t ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1), "c"(a2), "d"(a3) : "memory");
    return ret;
}

#endif /* _MOSS_SYSCALL_H */
