/* 
* The MOSS Kernel's Round-Robin Scheduler
* Miro Haapalainen, 2026
*/

#include <string.h>
#define _ALLOC_SKIP_DEFINE
#include <liballoc.h>
#include <kernel/sched.h>

/* Update the TSS kernel stack on every context switch. */
extern void tss_set_kernel_stack(uint32_t esp0);

/* Per-process page directory switching. */
extern uint32_t *paging_get_page_dir(void);
extern void paging_switch_directory(uint32_t *pd);

typedef struct sched_list_s {
    task_t t;
    struct sched_list_s *next;
    struct sched_list_s *prev;
} sched_list_t;

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
    t.exit_code = 0;
    t.entry    = entry;
    t.brk_start   = 0;
    t.brk_current = 0;

    /* Signal handling defaults */
    t.sig_pending = 0;
    t.sig_blocked = 0;
    t.in_signal   = 0;
    for (int i = 0; i < _NSIG; i++)
        t.sig_handlers[i] = SIG_DFL;

    /* Per-process paging defaults */
    t.page_dir = NULL;           /* NULL = use kernel_page_dir */
    t.user_pages_slot = -1;      /* no user pages by default */
    t.user_entry_eip = 0;
    t.user_entry_esp = 0;
    t.parent_pid = 0;

    /* Fork child state (zeroed) */
    t.fork_eip = 0;
    t.fork_esp = 0;
    t.fork_eflags = 0;
    t.fork_ebx = t.fork_ecx = t.fork_edx = 0;
    t.fork_esi = t.fork_edi = t.fork_ebp = 0;
    t.fork_ds = 0;
    t.fork_gs = 0;

    /* TLS defaults (no TLS) */
    t.tls_gs    = 0;
    t.tls_entry = 0;
    t.tls_base  = 0;
    t.tls_limit = 0;

    /* User/group identity — default to root */
    t.uid  = 0;
    t.gid  = 0;
    t.euid = 0;
    t.egid = 0;

    /* Per-process cwd: initialize from global cwd */
    extern uint32_t cwd_ino;
    extern char cwd_path[256];
    t.cwd_ino = cwd_ino;
    memcpy(t.cwd_path, cwd_path, 256);

    /* Initialize file descriptor table */
    memset(t.fd_table, 0, sizeof(t.fd_table));
    /* fd 0 = stdin */
    t.fd_table[0].type  = FD_TYPE_STDIN;
    t.fd_table[0].flags = FD_FLAG_USED | FD_FLAG_READABLE;
    /* fd 1 = stdout */
    t.fd_table[1].type  = FD_TYPE_STDOUT;
    t.fd_table[1].flags = FD_FLAG_USED | FD_FLAG_WRITABLE;
    /* fd 2 = stderr */
    t.fd_table[2].type  = FD_TYPE_STDERR;
    t.fd_table[2].flags = FD_FLAG_USED | FD_FLAG_WRITABLE;

    if (entry) {
        /* Dynamically allocate a kernel stack */
        t.stack = (uint32_t *)malloc(TASK_STACK_SIZE);
        if (t.stack == NULL) {
            t.esp = 0;
            return t;
        }

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
    sched_list_t *new_node = (sched_list_t *)malloc(sizeof(sched_list_t));
    if (new_node == NULL)
        return;

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
            if (node->t.stack != NULL)
                free(node->t.stack);
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
 *
 * The idle task (pid 0) is only chosen if no other task is ready.
 */
void schedule(void) {
    if (sched_list_head == NULL)
        return;

    sched_list_t *prev_node = current_node;

    /* If there's a current task that was running, mark it ready */
    if (current_node != NULL && current_node->t.state == TASK_RUNNING)
        current_node->t.state = TASK_READY;

    /* Walk the circular list starting from the node after current.
     * Skip the idle task (pid 0) on the first pass; only fall back to
     * it if nothing else is runnable. */
    sched_list_t *start = (current_node != NULL) ? current_node->next
                                                  : sched_list_head;
    sched_list_t *candidate = start;
    sched_list_t *idle_node = NULL;
    do {
        if (candidate->t.state == TASK_READY) {
            if (candidate->t.pid == 0) {
                idle_node = candidate; /* remember but skip */
            } else {
                current_node = candidate;
                goto found;
            }
        }
        candidate = candidate->next;
    } while (candidate != start);

    /* No non-idle task found — use idle if available */
    if (idle_node) {
        current_node = idle_node;
        goto found;
    }

    /* No ready task found — if current is still runnable, keep it */
    if (current_node != NULL &&
        (current_node->t.state == TASK_READY ||
         current_node->t.state == TASK_RUNNING)) {
        current_node->t.state = TASK_RUNNING;
        current_node->t.counter = current_node->t.priority;
    }
    return;

found:
    current_node->t.state = TASK_RUNNING;
    current_node->t.counter = current_node->t.priority;

    /* Perform actual CPU context switch */
    if (prev_node != NULL && prev_node != current_node) {
        /* Save current task's cwd into its task struct,
         * and restore new task's cwd into globals. */
        extern uint32_t cwd_ino;
        extern char cwd_path[256];
        prev_node->t.cwd_ino = cwd_ino;
        memcpy(prev_node->t.cwd_path, cwd_path, 256);
        cwd_ino = current_node->t.cwd_ino;
        memcpy(cwd_path, current_node->t.cwd_path, 256);

        /* Point TSS.esp0 at the top of the new task's kernel stack
         * so interrupts from ring 3 land on the right stack. */
        if (current_node->t.stack != NULL)
            tss_set_kernel_stack((uint32_t)current_node->t.stack
                                 + TASK_STACK_SIZE);

        /* Switch to the new task's page directory — but skip the
         * expensive CR3 write if both tasks share the same directory. */
        uint32_t *new_pd = current_node->t.page_dir;
        if (!new_pd)
            new_pd = paging_get_page_dir();
        uint32_t *old_pd = prev_node->t.page_dir;
        if (!old_pd)
            old_pd = paging_get_page_dir();
        if (new_pd != old_pd)
            paging_switch_directory(new_pd);

        /* Restore new task's TLS GDT entry so %gs works */
        if (current_node->t.tls_gs) {
            extern int gdt_set_tls(int entry_number,
                                   uint32_t base, uint32_t limit);
            gdt_set_tls(current_node->t.tls_entry,
                        current_node->t.tls_base,
                        current_node->t.tls_limit);
        }

        switch_context(&prev_node->t.esp, current_node->t.esp);
    }
}

/* Called by the timer interrupt handler each tick. */
void timer_tick(void) {
    if (current_node == NULL)
        return;

    /* Wake all TASK_INTERRUPTIBLE tasks on each tick so they can
     * re-check their wait conditions (pipe data, sleep timeout, etc.) */
    sched_list_t *node = sched_list_head;
    if (node) {
        do {
            if (node->t.state == TASK_INTERRUPTIBLE)
                node->t.state = TASK_READY;
            node = node->next;
        } while (node != sched_list_head);
    }

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

/* Find a task by PID. Returns NULL if not found. */
task_t *find_task_by_pid(uint32_t pid) {
    if (sched_list_head == NULL)
        return NULL;

    sched_list_t *node = sched_list_head;
    do {
        if (node->t.pid == pid)
            return &node->t;
        node = node->next;
    } while (node != sched_list_head);

    return NULL;
}

/* Find any child of parent_pid. If want_zombie is set, prefer zombie children.
 * Returns task pointer, or NULL if no matching child exists.
 * Sets *found_any=1 if any child (zombie or not) was found. */
task_t *find_child_task(uint32_t parent_pid, int want_zombie, int *found_any) {
    if (sched_list_head == NULL)
        return NULL;

    task_t *any_child = NULL;
    sched_list_t *node = sched_list_head;
    do {
        if (node->t.parent_pid == parent_pid) {
            if (found_any) *found_any = 1;
            if (want_zombie && node->t.state == TASK_ZOMBIE)
                return &node->t;
            if (!any_child)
                any_child = &node->t;
        }
        node = node->next;
    } while (node != sched_list_head);

    return want_zombie ? NULL : any_child;
}

/* Send a signal to a task. Returns 0 on success, -1 on error. */
int task_send_signal(task_t *task, int sig) {
    if (!task || sig < 1 || sig >= _NSIG)
        return -1;

    /* SIGKILL and SIGSTOP cannot be caught or ignored */
    if (sig == SIGKILL) {
        task->sig_pending |= (1u << sig);
        /* Force-kill: if blocked or interruptible, wake it */
        if (task->state == TASK_BLOCKED || task->state == TASK_INTERRUPTIBLE)
            task->state = TASK_READY;
        return 0;
    }

    /* Check if signal is ignored */
    sighandler_t handler = task->sig_handlers[sig];
    if (handler == SIG_IGN)
        return 0;  /* silently discard */

    /* Set the pending bit */
    task->sig_pending |= (1u << sig);

    /* Wake task if it's in an interruptible sleep */
    if (task->state == TASK_INTERRUPTIBLE)
        task->state = TASK_READY;

    /* SIGCONT unblocks stopped tasks */
    if (sig == SIGCONT && task->state == TASK_STOPPED)
        task->state = TASK_READY;

    return 0;
}
