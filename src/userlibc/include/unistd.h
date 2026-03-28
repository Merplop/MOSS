/*
 * MOSS user-space libc — <unistd.h>
 * POSIX-like system call wrappers.
 */
#ifndef _UNISTD_H
#define _UNISTD_H

#include <stdint.h>
#include <stddef.h>
#include <sys/types.h>

#define STDIN_FILENO  0
#define STDOUT_FILENO 1
#define STDERR_FILENO 2

#ifdef __cplusplus
extern "C" {
#endif

void _exit(int status) __attribute__((noreturn));
ssize_t write(int fd, const void *buf, size_t count);
ssize_t read(int fd, void *buf, size_t count);
pid_t getpid(void);
int open(const char *path, int flags, ...);
int close(int fd);
off_t lseek(int fd, off_t offset, int whence);
int brk(void *addr);
void *sbrk(intptr_t increment);
unsigned int sleep(unsigned int seconds);
int usleep(unsigned int ms);
unsigned int getticks(void);
char *getcwd(char *buf, size_t size);
int chdir(const char *path);
int unlink(const char *path);
int dup(int oldfd);
int dup2(int oldfd, int newfd);
int access(const char *path, int mode);
int execvp(const char *file, char *const argv[]);

/* access() mode flags */
#define F_OK 0
#define R_OK 4
#define W_OK 2
#define X_OK 1

#ifdef __cplusplus
}
#endif

#endif /* _UNISTD_H */
