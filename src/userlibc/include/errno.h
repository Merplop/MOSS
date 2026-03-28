/* <errno.h> — minimal errno for MOSS libc */
#ifndef _ERRNO_H
#define _ERRNO_H

extern int errno;

#define ENOENT   2
#define ENOMEM  12
#define EACCES  13
#define EEXIST  17
#define EISDIR  21
#define EINVAL  22
#define EMFILE  24
#define ENOSPC  28
#define ERANGE  34
#define EBADF    9
#define ENOSYS  38
#define EAGAIN  11
#define EINTR    4
#define EIO      5

#endif /* _ERRNO_H */
