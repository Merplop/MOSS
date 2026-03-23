#ifndef _KERNEL_SCHED_H
#define _KERNEL_SCHED_H

#define TASK_RUNNING         0
#define TASK_READY           1
#define TASK_BLOCKED         2
#define TASK_INTERRUPTIBLE   3
#define TASK_STOPPED         4
#define TASK_ZOMBIE          5

#define DEFAULT_QUANTUM      10
#define DEFAULT_PRIORITY     5

typedef struct task_s {
    long state;
    long counter;
    long priority;
    long signal;
    int exit_code;
} task_t;

void init_scheduler(void);
task_t task_create(long priority);
task_t *get_current_task(void);
int get_task_count(void);
void admit_task(task_t *new_task);
void remove_task(task_t *task_to_remove);
void schedule(void);
void timer_tick(void);
void block_task(task_t *task);
void unblock_task(task_t *task);
void exit_task(int exit_code);

#endif
