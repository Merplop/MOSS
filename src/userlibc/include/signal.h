/* <signal.h> — MOSS user libc signal support. */
#ifndef _SIGNAL_H
#define _SIGNAL_H

#include <syscall.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*sighandler_t)(int);
typedef int sig_atomic_t;
typedef unsigned int sigset_t;

#define SIG_DFL  ((sighandler_t)0)
#define SIG_IGN  ((sighandler_t)1)
#define SIG_ERR  ((sighandler_t)-1)

#define SIGHUP    1
#define SIGINT    2
#define SIGQUIT   3
#define SIGILL    4
#define SIGTRAP   5
#define SIGABRT   6
#define SIGBUS    7
#define SIGFPE    8
#define SIGKILL   9
#define SIGUSR1  10
#define SIGSEGV  11
#define SIGUSR2  12
#define SIGPIPE  13
#define SIGALRM  14
#define SIGTERM  15
#define SIGCHLD  17
#define SIGCONT  18
#define SIGSTOP  19
#define SIGTSTP  20
#define SIGTTIN  21
#define SIGTTOU  22

#define _NSIG    32

/* SIG_BLOCK / SIG_UNBLOCK / SIG_SETMASK for sigprocmask */
#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

static inline sighandler_t signal(int sig, sighandler_t handler) {
    return (sighandler_t)(uintptr_t)_syscall2(SYS_SIGNAL, (uint32_t)sig, (uint32_t)(uintptr_t)handler);
}

static inline int kill(int pid, int sig) {
    return (int)_syscall2(SYS_KILL, (uint32_t)pid, (uint32_t)sig);
}

static inline int raise(int sig) {
    return kill((int)_syscall0(SYS_GETPID), sig);
}

static inline int sigprocmask(int how, const sigset_t *set, sigset_t *oldset) {
    return (int)_syscall3(SYS_SIGPROCMASK, (uint32_t)how, (uint32_t)(uintptr_t)set, (uint32_t)(uintptr_t)oldset);
}

#ifdef __cplusplus
}
#endif

#endif /* _SIGNAL_H */
