/*
 * sync.c — Mutex and semaphore implementation.
 * MOSS Kernel
 *
 * Uses spinlocks to protect internal state and the scheduler for
 * sleep/wake behavior.  Mutexes and semaphores must NOT be used from
 * interrupt context (use spinlocks there instead).
 */

#include <kernel/sync.h>
#include <kernel/sched.h>
#include <stddef.h>

/* ------------------------------------------------------------------ */
/*  Mutex                                                              */
/* ------------------------------------------------------------------ */

void mutex_init(mutex_t *m) {
    m->locked = 0;
    m->owner = NULL;
    m->wait_count = 0;
    for (int i = 0; i < MUTEX_MAX_WAITERS; i++)
        m->wait_queue[i] = NULL;
}

void mutex_lock(mutex_t *m) {
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");

    task_t *self = get_current_task();

    while (m->locked) {
        /* Add ourselves to the wait queue */
        if (m->wait_count < MUTEX_MAX_WAITERS && self) {
            m->wait_queue[m->wait_count++] = self;
            self->state = TASK_BLOCKED;
        }
        /* Restore interrupts and yield */
        asm volatile("pushl %0; popfl" : : "r"(flags) : "memory");
        schedule();
        /* Re-disable interrupts for the next check */
        asm volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    }

    m->locked = 1;
    m->owner = self;

    asm volatile("pushl %0; popfl" : : "r"(flags) : "memory");
}

void mutex_unlock(mutex_t *m) {
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");

    m->locked = 0;
    m->owner = NULL;

    /* Wake all waiters — scheduler will pick one */
    for (int i = 0; i < m->wait_count; i++) {
        if (m->wait_queue[i] && m->wait_queue[i]->state == TASK_BLOCKED)
            m->wait_queue[i]->state = TASK_READY;
        m->wait_queue[i] = NULL;
    }
    m->wait_count = 0;

    asm volatile("pushl %0; popfl" : : "r"(flags) : "memory");
}

/* ------------------------------------------------------------------ */
/*  Semaphore                                                          */
/* ------------------------------------------------------------------ */

void sem_init(semaphore_t *s, int initial_value) {
    s->value = initial_value;
    s->wait_count = 0;
    for (int i = 0; i < SEM_MAX_WAITERS; i++)
        s->wait_queue[i] = NULL;
}

void sem_wait(semaphore_t *s) {
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");

    task_t *self = get_current_task();

    while (s->value <= 0) {
        /* Add ourselves to the wait queue */
        if (s->wait_count < SEM_MAX_WAITERS && self) {
            s->wait_queue[s->wait_count++] = self;
            self->state = TASK_BLOCKED;
        }
        asm volatile("pushl %0; popfl" : : "r"(flags) : "memory");
        schedule();
        asm volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    }

    s->value--;

    asm volatile("pushl %0; popfl" : : "r"(flags) : "memory");
}

void sem_post(semaphore_t *s) {
    uint32_t flags;
    asm volatile("pushfl; popl %0; cli" : "=r"(flags) : : "memory");

    s->value++;

    /* Wake one waiter */
    if (s->wait_count > 0) {
        task_t *waiter = s->wait_queue[0];
        /* Shift queue down */
        for (int i = 1; i < s->wait_count; i++)
            s->wait_queue[i - 1] = s->wait_queue[i];
        s->wait_queue[--s->wait_count] = NULL;

        if (waiter && waiter->state == TASK_BLOCKED)
            waiter->state = TASK_READY;
    }

    asm volatile("pushl %0; popfl" : : "r"(flags) : "memory");
}
