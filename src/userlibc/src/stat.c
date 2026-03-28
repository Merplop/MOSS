/*
 * MOSS user-space libc — stat/fstat/mkdir wrappers.
 */
#include <sys/stat.h>
#include <syscall.h>
#include <errno.h>

int stat(const char *path, struct stat *buf) {
    int ret = (int)_syscall2(SYS_STAT, (uint32_t)path, (uint32_t)buf);
    if (ret < 0) { errno = ENOENT; return -1; }
    return 0;
}

int fstat(int fd, struct stat *buf) {
    int ret = (int)_syscall2(SYS_FSTAT, (uint32_t)fd, (uint32_t)buf);
    if (ret < 0) { errno = EBADF; return -1; }
    return 0;
}

int mkdir(const char *path, mode_t mode) {
    (void)path; (void)mode;
    errno = ENOSYS;
    return -1;
}
