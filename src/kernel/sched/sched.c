/* 
* The MOSS Kernel's Round-Robin Scheduler
* Miro Haapalainen, 2026
*/

#include <liballoc.h>
#include <kernel/sched.h>

typedef struct sched_list_s {
    task_t t;
    struct sched_list_s *next;
    struct sched_list_s *prev;
} sched_list_t;

static sched_list_t *sched_list_head = NULL;
static sched_list_t *current_node = NULL;
static int task_count = 0;

void init_scheduler(void) {
    sched_list_head = NULL;
    current_node = NULL;
    task_count = 0;
}

task_t task_create(long priority) {
    task_t t;
    t.state = TASK_READY;
    t.counter = (priority > 0) ? priority : DEFAULT_QUANTUM;
    t.priority = (priority > 0) ? priority : DEFAULT_PRIORITY;
    t.signal = 0;
    t.exit_code = 0;
    return t;
}

task_t *get_current_task(void) {
    if (current_node == NULL)
        return NULL;
    return &current_node->t;
}

int get_task_count(void) {
    return task_count;
}

void admit_task(task_t *new_task) {
    sched_list_t *new_node = (sched_list_t *)malloc(sizeof(sched_list_t));
    if (new_node == NULL)
        return;

    new_node->t = *new_task;
    new_node->t.state = TASK_READY;

    if (sched_list_head == NULL) {
        sched_list_head = new_node;
        new_node->next = new_node;
        new_node->prev = new_node;
        current_node = new_node;
    } else {
        sched_list_t *tail = sched_list_head->prev;
        tail->next = new_node;
        new_node->prev = tail;
        new_node->next = sched_list_head;
        sched_list_head->prev = new_node;
    }
    task_count++;
}

void remove_task(task_t *task_to_remove) {
    if (sched_list_head == NULL) return;

    sched_list_t *node = sched_list_head;
    do {
        if (&node->t == task_to_remove) {
            if (node->next == node) {
                /* last task in the list */
                sched_list_head = NULL;
                current_node = NULL;
            } else {
                node->prev->next = node->next;
                node->next->prev = node->prev;
                if (sched_list_head == node)
                    sched_list_head = node->next;
                if (current_node == node)
                    current_node = node->next;
            }
            free(node);
            task_count--;
            return;
        }
        node = node->next;
    } while (node != sched_list_head);
}

/* Pick the next TASK_READY/TASK_RUNNING task in round-robin order. */
void schedule(void) {
    if (sched_list_head == NULL)
        return;

    /* If there's a current task that was running, mark it ready */
    if (current_node != NULL && current_node->t.state == TASK_RUNNING)
        current_node->t.state = TASK_READY;

    /* Walk the circular list starting from the node after current */
    sched_list_t *start = (current_node != NULL) ? current_node->next
                                                  : sched_list_head;
    sched_list_t *candidate = start;
    do {
        if (candidate->t.state == TASK_READY) {
            current_node = candidate;
            current_node->t.state = TASK_RUNNING;
            current_node->t.counter = current_node->t.priority;
            return;
        }
        candidate = candidate->next;
    } while (candidate != start);

    /* No ready task found — if current is still runnable, keep it */
    if (current_node != NULL &&
        (current_node->t.state == TASK_READY ||
         current_node->t.state == TASK_RUNNING)) {
        current_node->t.state = TASK_RUNNING;
        current_node->t.counter = current_node->t.priority;
    }
}

/* Called by the timer interrupt handler each tick. */
void timer_tick(void) {
    if (current_node == NULL)
        return;

    if (current_node->t.counter > 0)
        current_node->t.counter--;

    /* Quantum expired — reschedule */
    if (current_node->t.counter <= 0)
        schedule();
}

/* Mark a task as blocked (e.g. waiting on I/O). */
void block_task(task_t *task) {
    if (task == NULL) return;
    task->state = TASK_BLOCKED;
    if (get_current_task() == task)
        schedule();
}

/* Unblock a previously blocked task. */
void unblock_task(task_t *task) {
    if (task == NULL) return;
    if (task->state == TASK_BLOCKED)
        task->state = TASK_READY;
}

/* Mark a task as zombie (exited but not yet reaped). */
void exit_task(int exit_code) {
    task_t *current = get_current_task();
    if (current == NULL) return;
    current->state = TASK_ZOMBIE;
    current->exit_code = exit_code;
    schedule();
}



