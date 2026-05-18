/*
 * MOSS user-space syscall interface.
 * Inline wrappers around int $0x80.
 * Syscall numbers match Linux i386 ABI for musl compatibility.
 */
#ifndef _MOSS_SYSCALL_H
#define _MOSS_SYSCALL_H

#include <stdint.h>

/* Standard Linux i386 syscall numbers */
#define SYS_EXIT            1
#define SYS_FORK            2
#define SYS_READ            3
#define SYS_WRITE           4
#define SYS_OPEN            5
#define SYS_CLOSE           6
#define SYS_WAITPID         7
#define SYS_UNLINK         10
#define SYS_EXECVE         11
#define SYS_CHDIR          12
#define SYS_TIME           13
#define SYS_CHMOD          15
#define SYS_LSEEK          19
#define SYS_GETPID         20
#define SYS_ACCESS         33
#define SYS_KILL           37
#define SYS_RENAME         38
#define SYS_MKDIR          39
#define SYS_RMDIR          40
#define SYS_DUP            41
#define SYS_PIPE           42
#define SYS_BRK            45
#define SYS_GETGID         47
#define SYS_GETEUID        49
#define SYS_GETEGID        50
#define SYS_IOCTL          54
#define SYS_FCNTL          55
#define SYS_UMASK          60
#define SYS_DUP2           63
#define SYS_GETPPID        64
#define SYS_SETSID         66
#define SYS_READLINK       85
#define SYS_MUNMAP         91
#define SYS_WAIT4          114
#define SYS_CLONE          120
#define SYS_WRITEV         146
#define SYS_NANOSLEEP      162
#define SYS_RT_SIGACTION   174
#define SYS_RT_SIGPROCMASK 175
#define SYS_GETCWD         183
#define SYS_MMAP2          192
#define SYS_STAT64         195
#define SYS_LSTAT64        196
#define SYS_FSTAT64        197
#define SYS_GETUID32       199
#define SYS_GETGID32       200
#define SYS_GETEUID32      201
#define SYS_GETEGID32      202
#define SYS_GETDENTS64     220
#define SYS_FCNTL64        221
#define SYS_SET_THREAD_AREA 243
#define SYS_EXIT_GROUP     252
#define SYS_CLOCK_GETTIME  265
#define SYS_OPENAT         295
#define SYS_PIPE2          331

/* Additional Linux syscalls used by userlibc */
#define SYS_STAT           106
#define SYS_FSTAT          108
#define SYS_SIGNAL         48
#define SYS_SIGPROCMASK    175  /* rt_sigprocmask */
#define SYS_SETENV         511
#define SYS_GETENV         512

/* MOSS-specific syscalls */
#define SYS_FBMAP          500
#define SYS_GETTICKS       501
#define SYS_WRITE_N        502
#define SYS_POLL_KEY       503
#define SYS_WAIT_KEY       504
#define SYS_GET_WINSIZE    505
#define SYS_FB_FLUSH       506
#define SYS_AUDIO_WRITE    507
#define SYS_AUDIO_AVAIL    508
#define SYS_AUDIO_START    509
#define SYS_ISATTY         510

/* open() flags */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0040
#define O_TRUNC   0x0200
#define O_APPEND  0x0400

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

static inline int32_t _syscall4(uint32_t num, uint32_t a1, uint32_t a2,
                                 uint32_t a3, uint32_t a4) {
    int32_t ret;
    asm volatile("int $0x80" : "=a"(ret)
                 : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4) : "memory");
    return ret;
}

static inline int32_t _syscall5(uint32_t num, uint32_t a1, uint32_t a2,
                                 uint32_t a3, uint32_t a4, uint32_t a5) {
    int32_t ret;
    asm volatile("int $0x80" : "=a"(ret)
                 : "a"(num), "b"(a1), "c"(a2), "d"(a3), "S"(a4), "D"(a5)
                 : "memory");
    return ret;
}

#endif /* _MOSS_SYSCALL_H */
