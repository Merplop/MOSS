/* <signal.h> — MOSS user libc stub.
 * MOSS has no signal delivery, but TCC references these symbols. */
#ifndef _SIGNAL_H
#define _SIGNAL_H

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

static inline sighandler_t signal(int sig, sighandler_t handler) {
    (void)sig; (void)handler;
    return SIG_DFL;
}

static inline int raise(int sig) {
    (void)sig;
    return 0;
}

#ifdef __cplusplus
}
#endif

#endif /* _SIGNAL_H */
