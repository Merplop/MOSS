#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

#include <stdint.h>

/*
 * System call numbers — Linux i386 ABI.
 * Convention:
 *   eax = syscall number (and return value)
 *   ebx = arg1, ecx = arg2, edx = arg3, esi = arg4, edi = arg5, ebp = arg6
 *
 * Using standard Linux syscall numbers so musl libc works unmodified.
 */

/* Standard Linux i386 syscall numbers */
#define SYS_EXIT            1
#define SYS_FORK            2
#define SYS_READ            3
#define SYS_WRITE           4
#define SYS_OPEN            5
#define SYS_CLOSE           6
#define SYS_WAITPID         7
#define SYS_LINK           9
#define SYS_UNLINK         10
#define SYS_EXECVE         11
#define SYS_CHDIR          12
#define SYS_TIME           13
#define SYS_MKNOD          14
#define SYS_CHMOD          15
#define SYS_LSEEK          19
#define SYS_GETPID         20
#define SYS_SETUID         23
#define SYS_GETUID         24
#define SYS_ALARM          27
#define SYS_ACCESS         33
#define SYS_KILL           37
#define SYS_RENAME         38
#define SYS_MKDIR          39
#define SYS_RMDIR          40
#define SYS_DUP            41
#define SYS_PIPE           42
#define SYS_TIMES          43
#define SYS_BRK            45
#define SYS_SETGID         46
#define SYS_GETGID         47
#define SYS_SIGNAL         48
#define SYS_GETEUID        49
#define SYS_GETEGID        50
#define SYS_IOCTL          54
#define SYS_FCNTL          55
#define SYS_SETPGID        57
#define SYS_UMASK          60
#define SYS_DUP2           63
#define SYS_GETPPID        64
#define SYS_SETSID         66
#define SYS_SIGACTION      67
#define SYS_GETPGRP        65
#define SYS_GETRLIMIT      76
#define SYS_GETRUSAGE      77
#define SYS_GETTIMEOFDAY   78
#define SYS_SELECT         82
#define SYS_READLINK       85
#define SYS_MMAP           90
#define SYS_MUNMAP         91
#define SYS_SETITIMER      104
#define SYS_GETITIMER      105
#define SYS_FCHMOD         94
#define SYS_FCHOWN         95
#define SYS_STAT           106
#define SYS_FSTAT          108
#define SYS_SYSINFO        116
#define SYS_WAIT4          114
#define SYS_CLONE          120
#define SYS_UNAME          122
#define SYS_MPROTECT       125
#define SYS_SIGALTSTACK    186
#define SYS_NEWSELECT      142
#define SYS_READV          145
#define SYS_WRITEV         146
#define SYS_NANOSLEEP      162
#define SYS_POLL           168
#define SYS_RT_SIGRETURN   173
#define SYS_RT_SIGACTION   174
#define SYS_RT_SIGPROCMASK 175
#define SYS_RT_SIGSUSPEND  179
#define SYS_GETCWD         183
#define SYS_UGETRLIMIT     191
#define SYS_MMAP2          192
#define SYS_SETRLIMIT      75
#define SYS_STAT64         195
#define SYS_LSTAT64        196
#define SYS_FSTAT64        197
#define SYS_GETUID32       199
#define SYS_GETGID32       200
#define SYS_GETEUID32      201
#define SYS_GETEGID32      202
#define SYS_SETREUID32     203
#define SYS_SETREGID32     204
#define SYS_GETGROUPS32    205
#define SYS_SETGROUPS32    206
#define SYS_SETUID32       213
#define SYS_SETGID32       214
#define SYS_GETPGID        132
#define SYS_SETRESUID32    208
#define SYS_GETRESUID32    209
#define SYS_SETRESGID32    210
#define SYS_GETRESGID32    211
#define SYS_CHOWN32        212
#define SYS_GETDENTS64     220
#define SYS_FCNTL64        221
#define SYS_GETTID         224
#define SYS_SET_THREAD_AREA 243
#define SYS_SET_TID_ADDRESS 258
#define SYS_EXIT_GROUP     252
#define SYS_CLOCK_GETTIME  265
#define SYS_OPENAT         295
#define SYS_FSTATAT64      300
#define SYS_UNLINKAT       301
#define SYS_RENAMEAT       302
#define SYS_FCHMODAT       306
#define SYS_FACCESSAT      307
#define SYS_FCHOWNAT       298
#define SYS_PSELECT6       308
#define SYS_PIPE2          331
#define SYS_DUP3           330
#define SYS_CLOCK_GETTIME64 403

/* MOSS-specific syscalls (above Linux range) */
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
#define SYS_SETENV         511
#define SYS_GETENV         512
#define SYS_POLL_MOUSE     513
#define SYS_GET_MOUSE_POS  514

#define NUM_SYSCALLS 515

/* open() flags (matching Linux values) */
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0040
#define O_EXCL      0x0080
#define O_TRUNC     0x0200
#define O_APPEND    0x0400
#define O_NONBLOCK  0x0800
#define O_DIRECTORY 0x10000
#define O_CLOEXEC   0x80000

/* openat() special fd */
#define AT_FDCWD    (-100)

/* lseek() whence values */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* access() mode flags */
#define F_OK  0
#define R_OK  4
#define W_OK  2
#define X_OK  1

/* mmap prot flags */
#define PROT_NONE   0x0
#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define PROT_EXEC   0x4

/* mmap flags */
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_FIXED     0x10
#define MAP_ANONYMOUS 0x20
#define MAP_ANON      MAP_ANONYMOUS

#define MAP_FAILED    ((void *)-1)

/* clock IDs */
#define CLOCK_REALTIME  0
#define CLOCK_MONOTONIC 1

/* ioctl requests for terminal */
#define TCGETS      0x5401
#define TCSETS      0x5402
#define TCSETSW     0x5403
#define TCSETSF     0x5404
#define TIOCGWINSZ  0x5413
#define TIOCSWINSZ  0x5414
#define TIOCGPGRP   0x540F
#define TIOCSPGRP   0x5410

/* fcntl commands */
#define F_DUPFD     0
#define F_GETFD     1
#define F_SETFD     2
#define F_GETFL     3
#define F_SETFL     4
#define F_DUPFD_CLOEXEC 1030
#define FD_CLOEXEC  1

/* Signal constants */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

/* Initialise the syscall dispatcher (registers int 0x80 handler). */
void syscall_init(void);

#endif /* _KERNEL_SYSCALL_H */
