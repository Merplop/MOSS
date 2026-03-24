/* 
* The MOSS Kernel's Round-Robin Scheduler
* Miro Haapalainen, 2026
*/

#include <liballoc.h>
#include <kernel/sched.h>

/*
 * Static pool for scheduler nodes and task stacks.
 *
 * The kernel's physical memory manager (allocate_blocks) is initialised
 * with mbd->mmap_length which is the size of the GRUB memory-map
 * *buffer* — typically ~100 bytes — so max_blocks ends up 0 and every
 * allocation via malloc / allocate_blocks returns NULL.
 *
 * Until the memory manager is fixed, we use small BSS pools so the
 * scheduler can function without any dynamic allocation.
 */
#define MAX_STATIC_TASKS 16

typedef struct sched_list_s {
    task_t t;
    struct sched_list_s *next;
    struct sched_list_s *prev;
} sched_list_t;

static sched_list_t  static_nodes[MAX_STATIC_TASKS];
static int           static_nodes_used = 0;

static uint8_t static_stacks[MAX_STATIC_TASKS][TASK_STACK_SIZE]
    __attribute__((aligned(16)));
static int     static_stacks_used = 0;

static sched_list_t *sched_list_head = NULL;
static sched_list_t *current_node = NULL;
static int task_count = 0;
static uint32_t next_pid = 0;

/*
 * Trampoline for newly created tasks.
 * switch_context's `ret` lands here on the first run of a task.
 * We enable interrupts (the switch happened inside an IRQ handler
 * where IF was cleared) and call the task's real entry function.
 */
static void task_trampoline(void) {
    asm volatile("sti");
    task_t *self = get_current_task();
    if (self && self->entry)
        self->entry();
    exit_task(0);
    for (;;) asm volatile("hlt");
}

void init_scheduler(void) {
    sched_list_head = NULL;
    current_node = NULL;
    task_count = 0;
    next_pid = 0;
}

/*
 * Create a new task.
 *   entry != NULL  →  allocate a kernel stack and set up the initial
 *                      context so switch_context will "return" into
 *                      task_trampoline, which calls entry().
 *   entry == NULL  →  boot / idle task that already owns the current
 *                      stack.  Its esp will be filled in the first
 *                      time switch_context saves it.
 */
task_t task_create(void (*entry)(void), long priority) {
    task_t t;
    t.pid      = next_pid++;
    t.state    = TASK_READY;
    t.counter  = (priority > 0) ? priority : DEFAULT_QUANTUM;
    t.priority = (priority > 0) ? priority : DEFAULT_PRIORITY;
    t.signal   = 0;
    t.exit_code = 0;
    t.entry    = entry;

    if (entry) {
        /* Grab a kernel stack from the static pool */
        if (static_stacks_used >= MAX_STATIC_TASKS) {
            /* No stacks left — return a task that will never run */
            t.stack = NULL;
            t.esp   = 0;
            return t;
        }
        t.stack = (uint32_t *)static_stacks[static_stacks_used++];

        /*
         * Build the initial stack frame that switch_context expects.
         *
         *   top of block  → [dummy ret addr]   (ABI alignment padding)
         *                   [task_trampoline]   ← switch_context's `ret`
         *                   [0]                 ← ebp
         *                   [0]                 ← ebx
         *                   [0]                 ← esi
         *                   [0]                 ← edi  ← initial ESP
         */
        uint32_t *sp = (uint32_t *)((uint32_t)t.stack + TASK_STACK_SIZE);
        *(--sp) = 0;                              /* dummy return addr   */
        *(--sp) = (uint32_t)task_trampoline;      /* ret destination     */
        *(--sp) = 0;   /* ebp */
        *(--sp) = 0;   /* ebx */
        *(--sp) = 0;   /* esi */
        *(--sp) = 0;   /* edi */
        t.esp = (uint32_t)sp;
    } else {
        /* Boot task — uses the current stack */
        t.stack = NULL;
        t.esp   = 0;
    }

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
    sched_list_t *new_node;

    /* Try the static pool first; fall back to malloc. */
    if (static_nodes_used < MAX_STATIC_TASKS) {
        new_node = &static_nodes[static_nodes_used++];
    } else {
        new_node = (sched_list_t *)malloc(sizeof(sched_list_t));
        if (new_node == NULL)
            return;
    }

    new_node->t = *new_task;

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
            /* Only free dynamically allocated nodes (not from static pool) */
            if (node < &static_nodes[0] ||
                node >= &static_nodes[MAX_STATIC_TASKS])
                free(node);
            task_count--;
            return;
        }
        node = node->next;
    } while (node != sched_list_head);
}

/*
 * Pick the next TASK_READY task in round-robin order and perform a
 * context switch if the selected task differs from the current one.
 */
void schedule(void) {
    if (sched_list_head == NULL)
        return;

    sched_list_t *prev_node = current_node;

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

            /* Perform actual CPU context switch */
            if (prev_node != NULL && prev_node != current_node)
                switch_context(&prev_node->t.esp, current_node->t.esp);

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



