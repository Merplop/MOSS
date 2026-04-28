#ifndef _KERNEL_SCHED_H
#define _KERNEL_SCHED_H

#include <stdint.h>

#define TASK_RUNNING         0
#define TASK_READY           1
#define TASK_BLOCKED         2
#define TASK_INTERRUPTIBLE   3
#define TASK_STOPPED         4
#define TASK_ZOMBIE          5

#define DEFAULT_QUANTUM      10
#define DEFAULT_PRIORITY     5
#define TASK_STACK_SIZE      16384

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

typedef struct {
    uint8_t  type;       /* FD_TYPE_* */
    uint8_t  flags;      /* FD_FLAG_* */
    uint32_t ino;        /* ext2 inode number (for FD_TYPE_FILE) */
    uint32_t offset;     /* current file offset */
    uint32_t file_size;  /* cached file size at open time */
} fd_entry_t;

typedef struct task_s {
    uint32_t esp;            /* saved stack pointer */
    uint32_t pid;
    long state;
    long counter;
    long priority;
    long signal;
    int exit_code;
    uint32_t *stack;         /* base of allocated kernel stack (NULL for boot task) */
    void (*entry)(void);     /* entry function for new tasks */
    uint32_t brk_start;      /* initial program break (page-aligned end of loaded segments) */
    uint32_t brk_current;    /* current program break */
    fd_entry_t fd_table[MAX_OPEN_FILES];

    /* Per-process paging */
    uint32_t *page_dir;      /* this task's page directory (NULL = kernel_page_dir) */
    int user_pages_slot;     /* index into per-task user page tracking (-1 = none) */

    /* Parent/child relationship */
    uint32_t parent_pid;     /* PID of the parent task (for waitpid) */

    /* Fork child saved user-mode state */
    uint32_t fork_eip;
    uint32_t fork_esp;
    uint32_t fork_eflags;
    uint32_t fork_ebx, fork_ecx, fork_edx;
    uint32_t fork_esi, fork_edi, fork_ebp;
    uint32_t fork_ds;
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

/* Assembly context switch (defined in arch/i386/switch.S) */
extern void switch_context(uint32_t *old_esp, uint32_t new_esp);

#endif
