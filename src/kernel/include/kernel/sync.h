/*
 * sync.h — Kernel synchronization primitives (single-CPU).
 *
 * Provides:
 *   spinlock_t  — interrupt-disabling lock for short critical sections
 *   mutex_t     — sleeping lock for longer critical sections
 *   semaphore_t — counting semaphore
 */
#ifndef _KERNEL_SYNC_H
#define _KERNEL_SYNC_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Spinlock (interrupt-safe, non-sleeping)                            */
/* ------------------------------------------------------------------ */

typedef struct {
    volatile uint32_t locked;   /* 1 = held, 0 = free */
    uint32_t          eflags;   /* saved EFLAGS (IF bit) at lock time */
} spinlock_t;

#define SPINLOCK_INIT { 0, 0 }

static inline void spin_lock_init(spinlock_t *lock) {
    lock->locked = 0;
    lock->eflags = 0;
}

/*
 * Acquire spinlock: save interrupt state, disable interrupts, set locked.
 * On a single-CPU system this is sufficient — no other code can run while
 * interrupts are off and we hold the lock.
 */
static inline void spin_lock(spinlock_t *lock) {
    uint32_t flags;
    asm volatile(
        "pushfl\n\t"
        "popl %0\n\t"
        "cli"
        : "=r"(flags)
        :
        : "memory"
    );
    lock->eflags = flags;
    lock->locked = 1;
}

/*
 * Release spinlock: clear locked, restore saved interrupt state.
 */
static inline void spin_unlock(spinlock_t *lock) {
    uint32_t flags = lock->eflags;
    lock->locked = 0;
    asm volatile(
        "pushl %0\n\t"
        "popfl"
        :
        : "r"(flags)
        : "memory"
    );
}

/* ------------------------------------------------------------------ */
/*  Mutex (sleeping lock, not usable in interrupt context)             */
/* ------------------------------------------------------------------ */

#define MUTEX_MAX_WAITERS 16

typedef struct {
    volatile uint32_t locked;                       /* 1 = held */
    struct task_s    *owner;                         /* current holder */
    struct task_s    *wait_queue[MUTEX_MAX_WAITERS]; /* tasks waiting */
    int               wait_count;
} mutex_t;

#define MUTEX_INIT { 0, 0, {0}, 0 }

void mutex_init(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

/* ------------------------------------------------------------------ */
/*  Semaphore (counting, sleeping)                                     */
/* ------------------------------------------------------------------ */

#define SEM_MAX_WAITERS 16

typedef struct {
    volatile int       value;
    struct task_s     *wait_queue[SEM_MAX_WAITERS];
    int                wait_count;
} semaphore_t;

void sem_init(semaphore_t *s, int initial_value);
void sem_wait(semaphore_t *s);    /* P / down */
void sem_post(semaphore_t *s);    /* V / up   */

#endif /* _KERNEL_SYNC_H */
