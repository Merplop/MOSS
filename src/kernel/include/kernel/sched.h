#ifndef _KERNEL_SCHED_H
#define _KERNEL_SCHED_H

#include <stdint.h>

#define TASK_RUNNING         0
#define TASK_READY           1
#define TASK_BLOCKED         2
#define TASK_INTERRUPTIBLE   3
#define TASK_STOPPED         4
#define TASK_ZOMBIE          5

#define DEFAULT_QUANTUM      100
#define DEFAULT_PRIORITY     5
#define TASK_STACK_SIZE      16384

/* ------------------------------------------------------------------ */
/*  Signal definitions                                                 */
/* ------------------------------------------------------------------ */

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
#define SIGWINCH 28
#define _NSIG    32

#define SIG_DFL  ((void (*)(int))0)
#define SIG_IGN  ((void (*)(int))1)
#define SIG_ERR  ((void (*)(int))-1)

typedef void (*sighandler_t)(int);

/* ------------------------------------------------------------------ */
/*  Per-process file descriptor table                                  */
/* ------------------------------------------------------------------ */

#define MAX_OPEN_FILES 16

/* Flags for fd_entry.flags */
#define FD_FLAG_USED     0x01
#define FD_FLAG_READABLE 0x02
#define FD_FLAG_WRITABLE 0x04

/* Special fd types */
#define FD_TYPE_NONE     0
#define FD_TYPE_STDIN    1
#define FD_TYPE_STDOUT   2
#define FD_TYPE_STDERR   3
#define FD_TYPE_FILE     4
#define FD_TYPE_PIPE     5
#define FD_TYPE_DEVNULL  6
#define FD_TYPE_DEVTTY   7

typedef struct {
    uint8_t  type;       /* FD_TYPE_* */
    uint8_t  flags;      /* FD_FLAG_* */
    uint32_t ino;        /* ext2 inode number (for FD_TYPE_FILE) */
    uint32_t offset;     /* current file offset */
    uint32_t file_size;  /* cached file size at open time */
    uint32_t pipe_idx;   /* index into pipe table (for FD_TYPE_PIPE) */
} fd_entry_t;

typedef struct task_s {
    uint32_t esp;            /* saved stack pointer */
    uint32_t pid;
    long state;
    long counter;
    long priority;
    int exit_code;
    uint32_t *stack;         /* base of allocated kernel stack (NULL for boot task) */
    void (*entry)(void);     /* entry function for new tasks */
    uint32_t brk_start;      /* initial program break (page-aligned end of loaded segments) */
    uint32_t brk_current;    /* current program break */
    fd_entry_t fd_table[MAX_OPEN_FILES];

    /* Per-process paging */
    uint32_t *page_dir;      /* this task's page directory (NULL = kernel_page_dir) */
    int user_pages_slot;     /* index into per-task user page tracking (-1 = none) */

    /* User-mode entry point (set by ELF loader, read on first schedule) */
    uint32_t user_entry_eip;
    uint32_t user_entry_esp;

    /* Parent/child relationship */
    uint32_t parent_pid;     /* PID of the parent task (for waitpid) */

    /* Signal handling */
    uint32_t sig_pending;              /* bitmask of pending signals */
    uint32_t sig_blocked;              /* bitmask of blocked signals */
    sighandler_t sig_handlers[_NSIG];  /* per-signal handlers */
    int in_signal;                     /* non-zero if currently in a signal handler */

    /* Fork child saved user-mode state */
    uint32_t fork_eip;
    uint32_t fork_esp;
    uint32_t fork_eflags;
    uint32_t fork_ebx, fork_ecx, fork_edx;
    uint32_t fork_esi, fork_edi, fork_ebp;
    uint32_t fork_ds;
    uint32_t fork_gs;

    /* Per-task TLS (set by set_thread_area) */
    uint16_t tls_gs;       /* GS selector (0 = no TLS) */
    int      tls_entry;    /* GDT entry number (6-8) */
    uint32_t tls_base;     /* TLS segment base address */
    uint32_t tls_limit;    /* TLS segment limit */

    /* Per-process current working directory */
    uint32_t cwd_ino;
    char cwd_path[256];

    /* User/group identity */
    uint16_t uid;
    uint16_t gid;
    uint16_t euid;   /* effective uid (for setuid binaries) */
    uint16_t egid;   /* effective gid */
} task_t;

void init_scheduler(void);
task_t task_create(void (*entry)(void), long priority);
task_t *get_current_task(void);
int get_task_count(void);
void admit_task(task_t *new_task);
void remove_task(task_t *task_to_remove);
void schedule(void);
void timer_tick(void);
void block_task(task_t *task);
void unblock_task(task_t *task);
void exit_task(int exit_code);

/* Find a task by PID (returns NULL if not found). */
task_t *find_task_by_pid(uint32_t pid);

/* Find a child task of parent_pid. If want_zombie, returns first zombie child.
 * Sets *found_any=1 if any child exists. Returns NULL if no match. */
task_t *find_child_task(uint32_t parent_pid, int want_zombie, int *found_any);

/* Send a signal to a task. Returns 0 on success, -1 on error. */
int task_send_signal(task_t *task, int sig);

/* Assembly context switch (defined in arch/i386/switch.S) */
extern void switch_context(uint32_t *old_esp, uint32_t new_esp);

#endif
