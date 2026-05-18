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
    struct { uint32_t tv_sec; uint32_t tv_nsec; } req = { seconds, 0 };
    _syscall2(SYS_NANOSLEEP, (uint32_t)&req, 0);
    return 0;
}

int usleep(unsigned int us) {
    struct { uint32_t tv_sec; uint32_t tv_nsec; } req = { us / 1000000, (us % 1000000) * 1000 };
    return (int)_syscall2(SYS_NANOSLEEP, (uint32_t)&req, 0);
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

int pipe(int pipefd[2]) {
    int32_t ret = _syscall1(SYS_PIPE, (uint32_t)pipefd);
    if (ret < 0) { errno = EMFILE; return -1; }
    return 0;
}

pid_t fork(void) {
    int32_t ret = _syscall0(SYS_FORK);
    if (ret < 0) { errno = EAGAIN; return -1; }
    return (pid_t)ret;
}

pid_t waitpid(pid_t pid, int *status, int options) {
    (void)options;  /* options not supported yet */
    int32_t ret = _syscall2(SYS_WAITPID, (uint32_t)(int32_t)pid, (uint32_t)status);
    if (ret < 0) { errno = ECHILD; return -1; }
    return (pid_t)ret;
}

int execve(const char *path, char *const argv[], char *const envp[]) {
    (void)envp; /* environment not supported yet */
    int32_t ret = _syscall3(SYS_EXECVE, (uint32_t)path, (uint32_t)argv, (uint32_t)envp);
    /* execve only returns on error */
    errno = ENOENT;
    return (int)ret;
}

int execv(const char *path, char *const argv[]) {
    return execve(path, argv, (char *const *)0);
}

int execvp(const char *file, char *const argv[]) {
    if (!file) { errno = ENOENT; return -1; }

    /* If file contains a '/', treat as a direct path */
    for (const char *p = file; *p; p++) {
        if (*p == '/') {
            return execve(file, argv, (char *const *)0);
        }
    }

    /* Search PATH-like locations */
    static const char *search_dirs[] = { "/bin/", "/usr/bin/", "/" , NULL };
    char path_buf[256];

    for (int i = 0; search_dirs[i]; i++) {
        /* Build candidate path */
        const char *dir = search_dirs[i];
        size_t dlen = 0;
        while (dir[dlen]) dlen++;
        size_t flen = 0;
        while (file[flen]) flen++;
        if (dlen + flen + 1 > sizeof(path_buf))
            continue;

        for (size_t j = 0; j < dlen; j++) path_buf[j] = dir[j];
        for (size_t j = 0; j < flen; j++) path_buf[dlen + j] = file[j];
        path_buf[dlen + flen] = '\0';

        /* Try this path — if the file exists, execve it */
        if (access(path_buf, F_OK) == 0) {
            execve(path_buf, argv, (char *const *)0);
            /* If execve returned, there was an error loading it */
            return -1;
        }
    }

    errno = ENOENT;
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

int isatty(int fd) {
    return (int)_syscall1(SYS_ISATTY, (uint32_t)fd);
}

pid_t getppid(void) {
    return (pid_t)_syscall0(SYS_GETPPID);
}

int mkdir(const char *path, unsigned int mode) {
    (void)mode;
    return (int)_syscall1(SYS_MKDIR, (uint32_t)path);
}
