#ifndef _KERNEL_SYSCALL_H
#define _KERNEL_SYSCALL_H

#include <stdint.h>

/*
 * System call numbers.
 * Convention (i386 Linux-style):
 *   eax = syscall number (and return value)
 *   ebx = arg1, ecx = arg2, edx = arg3, esi = arg4, edi = arg5
 */
#define SYS_EXIT    0
#define SYS_WRITE   1
#define SYS_READ    2
#define SYS_GETPID  3
#define SYS_BRK     4
#define SYS_OPEN    5
#define SYS_CLOSE   6
#define SYS_LSEEK   7
#define SYS_FBMAP   8
#define SYS_GETTICKS 9
#define SYS_USLEEP  10
#define SYS_WRITE_N 11
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

#define NUM_SYSCALLS 23

/* open() flags (matching Linux values) */
#define O_RDONLY  0x0000
#define O_WRONLY  0x0001
#define O_RDWR    0x0002
#define O_CREAT   0x0040
#define O_TRUNC   0x0200

/* lseek() whence values */
#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

/* Initialise the syscall dispatcher (registers int 0x80 handler). */
void syscall_init(void);

#endif /* _KERNEL_SYSCALL_H */
