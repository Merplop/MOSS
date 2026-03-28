/*
 * MOSS user-space libc — POSIX-like syscall wrappers.
 */
#include <unistd.h>
#include <syscall.h>
#include <errno.h>

void _exit(int status) {
    _syscall1(SYS_EXIT, (uint32_t)status);
    __builtin_unreachable();
}

ssize_t write(int fd, const void *buf, size_t count) {
    ssize_t ret = _syscall3(SYS_WRITE, (uint32_t)fd, (uint32_t)buf, (uint32_t)count);
    if (ret < 0) { errno = -ret ? -ret : EIO; return -1; }
    return ret;
}

ssize_t read(int fd, void *buf, size_t count) {
    ssize_t ret = _syscall3(SYS_READ, (uint32_t)fd, (uint32_t)buf, (uint32_t)count);
    if (ret < 0) { errno = -ret ? -ret : EIO; return -1; }
    return ret;
}

pid_t getpid(void) {
    return (pid_t)_syscall0(SYS_GETPID);
}

int open(const char *path, int flags, ...) {
    /* mode argument is accepted but ignored (ext2 uses default perms) */
    int ret = (int)_syscall2(SYS_OPEN, (uint32_t)path, (uint32_t)flags);
    if (ret < 0) { errno = -ret ? -ret : EIO; return -1; }
    return ret;
}

int close(int fd) {
    int ret = (int)_syscall1(SYS_CLOSE, (uint32_t)fd);
    if (ret < 0) { errno = EBADF; return -1; }
    return ret;
}

off_t lseek(int fd, off_t offset, int whence) {
    off_t ret = (off_t)_syscall3(SYS_LSEEK, (uint32_t)fd, (uint32_t)offset, (uint32_t)whence);
    if (ret < 0) { errno = EINVAL; return -1; }
    return ret;
}

int brk(void *addr) {
    int32_t result = _syscall1(SYS_BRK, (uint32_t)addr);
    return (result == (int32_t)(uint32_t)addr) ? 0 : -1;
}

void *sbrk(intptr_t increment) {
    uint32_t old_brk = (uint32_t)_syscall1(SYS_BRK, 0);
    if (increment == 0)
        return (void *)old_brk;
    uint32_t new_brk = old_brk + (uint32_t)increment;
    int32_t result = _syscall1(SYS_BRK, new_brk);
    if ((uint32_t)result != new_brk)
        return (void *)-1;
    return (void *)old_brk;
}

unsigned int sleep(unsigned int seconds) {
    _syscall1(SYS_USLEEP, seconds * 1000);
    return 0;
}

int usleep(unsigned int ms) {
    return (int)_syscall1(SYS_USLEEP, ms);
}

unsigned int getticks(void) {
    return (unsigned int)_syscall0(SYS_GETTICKS);
}

char *getcwd(char *buf, size_t size) {
    int32_t ret = _syscall2(SYS_GETCWD, (uint32_t)buf, (uint32_t)size);
    if (ret == 0) { errno = ERANGE; return NULL; }
    return buf;
}

int chdir(const char *path) {
    int ret = (int)_syscall1(SYS_CHDIR, (uint32_t)path);
    if (ret < 0) { errno = ENOENT; return -1; }
    return 0;
}

int unlink(const char *path) {
    int ret = (int)_syscall1(SYS_UNLINK, (uint32_t)path);
    if (ret < 0) { errno = ENOENT; return -1; }
    return 0;
}

int dup(int oldfd) {
    int ret = (int)_syscall1(SYS_DUP, (uint32_t)oldfd);
    if (ret < 0) { errno = EBADF; return -1; }
    return ret;
}

int dup2(int oldfd, int newfd) {
    int ret = (int)_syscall2(SYS_DUP2, (uint32_t)oldfd, (uint32_t)newfd);
    if (ret < 0) { errno = EBADF; return -1; }
    return ret;
}

int execvp(const char *file, char *const argv[]) {
    (void)file; (void)argv;
    errno = ENOSYS;
    return -1;
}

int access(const char *path, int mode) {
    (void)mode;
    /* Use stat to check existence */
    int ret = (int)_syscall2(SYS_STAT, (uint32_t)path, (uint32_t)NULL);
    /* We just need to check if the path resolves, but we can't pass NULL stat buf.
     * Use a stack buffer instead. */
    uint8_t tmp_stat[36]; /* sizeof(moss_stat_t) = 9*4 = 36 */
    ret = (int)_syscall2(SYS_STAT, (uint32_t)path, (uint32_t)tmp_stat);
    if (ret < 0) { errno = ENOENT; return -1; }
    return 0;
}
