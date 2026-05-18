/*
 * System call dispatcher and handlers.
 * MOSS Kernel
 *
 * System calls are invoked via  int $0x80  with:
 *   eax = syscall number
 *   ebx = arg1, ecx = arg2, edx = arg3, esi = arg4, edi = arg5
 * Return value is placed in eax.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <kernel/syscall.h>
#include <kernel/sched.h>
#include <kernel/tty.h>
#include <kernel/elf.h>
#include <kernel/memory_manager.h>
#include <kernel/ext2.h>
#include <kernel/kernel.h>
#include <kernel/keyboard.h>
#include <kernel/mouse.h>
#include <kernel/users.h>

/* Kernel-safe strcmp (no libc strcmp in freestanding kernel) */
static int kstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}

/*
 * Register frame pushed by isr_common_stub (defined in arch/i386/idt.h).
 * Duplicated here so kernel/ code doesn't depend on arch/ include paths.
 */
struct isr_regs {
    uint32_t gs;                                          /* pushed by stub */
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;  /* pushal */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;              /* pushed by CPU */
};

typedef void (*isr_handler_t)(struct isr_regs *regs);
void isr_register_handler(uint8_t n, isr_handler_t handler);

/* ------------------------------------------------------------------ */
/*  Pipe infrastructure                                                */
/* ------------------------------------------------------------------ */

#define PIPE_BUF_SIZE  4096
#define MAX_PIPES      16

typedef struct {
    uint8_t  buf[PIPE_BUF_SIZE];
    uint32_t read_pos;      /* next byte to read */
    uint32_t write_pos;     /* next byte to write */
    uint32_t count;         /* bytes currently in buffer */
    int      readers;       /* number of open read-end fds */
    int      writers;       /* number of open write-end fds */
    int      in_use;        /* 1 if this pipe slot is allocated */
} pipe_t;

static pipe_t pipe_table[MAX_PIPES];

/* Allocate a new pipe. Returns pipe index or -1. */
static int pipe_alloc(void) {
    for (int i = 0; i < MAX_PIPES; i++) {
        if (!pipe_table[i].in_use) {
            pipe_table[i].in_use = 1;
            pipe_table[i].read_pos = 0;
            pipe_table[i].write_pos = 0;
            pipe_table[i].count = 0;
            pipe_table[i].readers = 0;
            pipe_table[i].writers = 0;
            return i;
        }
    }
    return -1;
}

/* Release a pipe slot when no readers and no writers remain. */
static void pipe_maybe_free(int idx) {
    if (idx < 0 || idx >= MAX_PIPES)
        return;
    pipe_t *p = &pipe_table[idx];
    if (p->readers == 0 && p->writers == 0) {
        p->in_use = 0;
    }
}

/* ------------------------------------------------------------------ */
/*  TTY line discipline (canonical / cooked mode)                      */
/* ------------------------------------------------------------------ */

#define TTY_LINE_SIZE   1024     /* max line editing buffer */
#define TTY_COOKED_SIZE 4096     /* completed-input ring buffer */

static struct {
    /* Editing buffer: line currently being composed */
    char edit_buf[TTY_LINE_SIZE];
    uint32_t edit_len;

    /* Cooked ring buffer: completed lines ready for read() */
    char cooked[TTY_COOKED_SIZE];
    uint32_t cooked_rpos;
    uint32_t cooked_wpos;
    uint32_t cooked_count;

    int eof_pending;  /* Ctrl+D on empty line → next read returns 0 */
} tty_ldisc;

/* Push bytes from the edit buffer into the cooked ring. */
static void tty_flush_edit(void) {
    for (uint32_t i = 0; i < tty_ldisc.edit_len; i++) {
        if (tty_ldisc.cooked_count >= TTY_COOKED_SIZE)
            break;  /* ring full — drop excess */
        tty_ldisc.cooked[tty_ldisc.cooked_wpos] = tty_ldisc.edit_buf[i];
        tty_ldisc.cooked_wpos = (tty_ldisc.cooked_wpos + 1) % TTY_COOKED_SIZE;
        tty_ldisc.cooked_count++;
    }
    tty_ldisc.edit_len = 0;
}

/* Read from the cooked ring into user buffer. Returns bytes copied. */
static int32_t tty_read_cooked(char *buf, uint32_t count) {
    uint32_t to_read = count;
    if (to_read > tty_ldisc.cooked_count)
        to_read = tty_ldisc.cooked_count;

    for (uint32_t i = 0; i < to_read; i++) {
        buf[i] = tty_ldisc.cooked[tty_ldisc.cooked_rpos];
        tty_ldisc.cooked_rpos = (tty_ldisc.cooked_rpos + 1) % TTY_COOKED_SIZE;
    }
    tty_ldisc.cooked_count -= to_read;
    return (int32_t)to_read;
}

/* ------------------------------------------------------------------ */
/*  Signal delivery                                                    */
/* ------------------------------------------------------------------ */

/*
 * Signal trampoline: a small piece of code that lives at a well-known
 * address on the user stack.  The kernel pushes this code when delivering
 * a signal, then diverts EIP to the user's signal handler.  When the
 * handler returns, it executes this trampoline which calls SYS_RT_SIGRETURN
 * to restore the original user-mode register state.
 *
 * The trampoline code (i386 machine code):
 *   mov $SYS_RT_SIGRETURN, %eax   ; B8 AD 00 00 00  (173 = 0xAD)
 *   int $0x80                      ; CD 80
 *   hlt                            ; F4 (should never reach)
 */
#define SIGTRAMP_SIZE 8
static const uint8_t signal_trampoline[SIGTRAMP_SIZE] = {
    0xB8, 0xAD, 0x00, 0x00, 0x00,  /* mov eax, 173 (SYS_RT_SIGRETURN) */
    0xCD, 0x80,                     /* int 0x80 */
    0xF4                            /* hlt (unreachable) */
};

/*
 * Saved register state pushed on user stack for SYS_SIGRETURN to restore.
 */
typedef struct {
    uint32_t eax, ebx, ecx, edx, esi, edi, ebp;
    uint32_t eip, eflags, esp;
} sigcontext_t;

/* Forward decl of paging helper for user stack writes */
extern uint32_t paging_get_physical_in(uint32_t *pd, uint32_t virt);
#define SIG_PAGE_SIZE 4096

/*
 * Push a 32-bit value onto the user stack (decrement sp, write value).
 * Returns the new sp, or 0 on failure.
 */
static uint32_t push_user32(uint32_t *pd, uint32_t sp, uint32_t value) {
    sp -= 4;
    uint32_t page_vaddr = sp & ~(SIG_PAGE_SIZE - 1);
    uint32_t phys = paging_get_physical_in(pd, page_vaddr);
    if (phys == 0)
        return 0;
    *(uint32_t *)(phys + (sp & (SIG_PAGE_SIZE - 1))) = value;
    return sp;
}

/*
 * Attempt to deliver one pending signal to the current task.
 * Called from the syscall dispatcher just before returning to user mode.
 *
 * If a signal is delivered, we modify the isr_regs to divert the task
 * to the signal handler.  A sigcontext + trampoline is pushed onto the
 * user stack so the handler can return via SYS_SIGRETURN.
 *
 * Returns 1 if a signal was delivered, 0 if nothing to do.
 */
static int deliver_signal(struct isr_regs *regs) {
    task_t *t = get_current_task();
    if (!t)
        return 0;

    /* Only deliver signals when returning to ring 3 */
    if ((regs->cs & 3) == 0)
        return 0;

    /* Don't nest signals */
    if (t->in_signal)
        return 0;

    /* Find first pending, unblocked signal */
    uint32_t deliverable = t->sig_pending & ~t->sig_blocked;
    if (deliverable == 0)
        return 0;

    int sig = 0;
    for (int i = 1; i < _NSIG; i++) {
        if (deliverable & (1u << i)) {
            sig = i;
            break;
        }
    }
    if (sig == 0)
        return 0;

    /* Clear pending bit */
    t->sig_pending &= ~(1u << sig);

    sighandler_t handler = t->sig_handlers[sig];

    /* SIG_IGN: discard */
    if (handler == SIG_IGN)
        return 0;

    /* SIG_DFL: default actions */
    if (handler == SIG_DFL) {
        switch (sig) {
        case SIGCHLD:
        case SIGCONT:
        case SIGUSR1:
        case SIGUSR2:
            /* Default: ignore */
            return 0;
        case SIGSTOP:
        case SIGTSTP:
        case SIGTTIN:
        case SIGTTOU:
            /* Default: stop the process */
            t->state = TASK_STOPPED;
            schedule();
            return 0;
        default:
            /* Default: terminate the process */
            elf_cleanup_process(t);
            /* Unblock parent if waiting */
            {
                task_t *parent = find_task_by_pid(t->parent_pid);
                if (parent && parent->state == TASK_BLOCKED)
                    unblock_task(parent);
            }
            exit_task(128 + sig);
            return 1;  /* won't actually return */
        }
    }

    /* User-defined handler: construct signal frame on user stack.
     *
     * User stack layout after delivery (growing downward):
     *   [sigcontext_t]        <- saved registers for sigreturn (ctx_addr)
     *   [trampoline code]     <- 8 bytes (trampoline_addr)
     *   --- align to 4 ---
     *   [ctx_addr]            <- pointer to sigcontext (for sigreturn to find)
     *   [signal number]       <- argument to handler
     *   [return addr]         <- trampoline_addr (handler's ret lands here)
     *   ... handler starts executing with ESP here ...
     */
    uint32_t *pd = t->page_dir;
    if (!pd)
        return 0;  /* no user address space (kernel task) */

    uint32_t sp = regs->useresp;

    /* Push sigcontext (saved state) */
    sigcontext_t ctx;
    ctx.eax    = regs->eax;
    ctx.ebx    = regs->ebx;
    ctx.ecx    = regs->ecx;
    ctx.edx    = regs->edx;
    ctx.esi    = regs->esi;
    ctx.edi    = regs->edi;
    ctx.ebp    = regs->ebp;
    ctx.eip    = regs->eip;
    ctx.eflags = regs->eflags;
    ctx.esp    = regs->useresp;

    /* Allocate space on user stack for sigcontext */
    sp -= sizeof(sigcontext_t);
    uint32_t ctx_addr = sp;
    {
        uint8_t *src = (uint8_t *)&ctx;
        for (uint32_t i = 0; i < sizeof(sigcontext_t); i++) {
            uint32_t addr = sp + i;
            uint32_t pv = addr & ~(SIG_PAGE_SIZE - 1);
            uint32_t ph = paging_get_physical_in(pd, pv);
            if (ph == 0) return 0;
            *(uint8_t *)(ph + (addr & (SIG_PAGE_SIZE - 1))) = src[i];
        }
    }

    /* Push trampoline code */
    sp -= SIGTRAMP_SIZE;
    uint32_t trampoline_addr = sp;
    {
        for (uint32_t i = 0; i < SIGTRAMP_SIZE; i++) {
            uint32_t addr = sp + i;
            uint32_t pv = addr & ~(SIG_PAGE_SIZE - 1);
            uint32_t ph = paging_get_physical_in(pd, pv);
            if (ph == 0) return 0;
            *(uint8_t *)(ph + (addr & (SIG_PAGE_SIZE - 1))) = signal_trampoline[i];
        }
    }

    /* Align stack to 4 bytes */
    sp &= ~3u;

    /* Push ctx_addr (for sigreturn to find at useresp + 4) */
    sp = push_user32(pd, sp, ctx_addr);
    if (sp == 0) return 0;

    /* Push signal number (handler's first argument) */
    sp = push_user32(pd, sp, (uint32_t)sig);
    if (sp == 0) return 0;

    /* Push return address (points to trampoline) */
    sp = push_user32(pd, sp, trampoline_addr);
    if (sp == 0) return 0;

    /* Divert execution to the handler */
    regs->eip     = (uint32_t)handler;
    regs->useresp = sp;
    /* Ensure IF is set */
    regs->eflags |= 0x200;

    t->in_signal = 1;

    return 1;
}

/* ------------------------------------------------------------------ */
/*  Individual syscall implementations                                 */
/* ------------------------------------------------------------------ */

/* SYS_EXIT (0): terminate the current task.
 *   ebx = exit code */
static int32_t sys_exit(struct isr_regs *regs) {
    int code = (int)regs->ebx;

    /* Clean up per-process user pages and page directory. */
    task_t *current = get_current_task();
    if (current)
        elf_cleanup_process(current);

    /* If a parent task (shell) is waiting via elf_load_and_exec, unblock it
     * ONLY if this is the actual child that elf_load_and_exec spawned. */
    extern uint32_t elf_get_waiting_child_pid(void);
    extern void elf_clear_waiting_parent(void);
    task_t *parent = elf_get_waiting_parent();
    if (parent && current && current->pid == elf_get_waiting_child_pid()) {
        unblock_task(parent);
        elf_clear_waiting_parent();
    }

    /* Also try to unblock the parent if it's waiting via waitpid */
    if (current) {
        task_t *ptask = find_task_by_pid(current->parent_pid);
        if (ptask) {
            /* Send SIGCHLD to parent */
            task_send_signal(ptask, SIGCHLD);
            if (ptask->state == TASK_BLOCKED && ptask != parent)
                unblock_task(ptask);
        }
    }

    exit_task(code);
    /* Should not return, but just in case: */
    return 0;
}

/* SYS_WRITE (1): write bytes to a file descriptor.
 *   ebx = fd
 *   ecx = pointer to buffer
 *   edx = number of bytes
 *   Returns: number of bytes written, or -1 on error. */
static int32_t sys_write(struct isr_regs *regs) {
    uint32_t fd    = regs->ebx;
    const char *buf = (const char *)regs->ecx;
    uint32_t count = regs->edx;

    if (buf == NULL)
        return -1;

    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES)
        return -1;

    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED) || !(f->flags & FD_FLAG_WRITABLE))
        return -1;

    /* stdout / stderr → terminal */
    if (f->type == FD_TYPE_STDOUT || f->type == FD_TYPE_STDERR ||
        f->type == FD_TYPE_DEVTTY) {
        terminal_write(buf, (size_t)count);
        return (int32_t)count;
    }

    /* /dev/null → discard */
    if (f->type == FD_TYPE_DEVNULL)
        return (int32_t)count;

    /* Regular file write */
    if (f->type == FD_TYPE_FILE) {
        int written = ext2_write_file(f->ino, buf, f->offset, count);
        if (written > 0) {
            f->offset += (uint32_t)written;
            if (f->offset > f->file_size)
                f->file_size = f->offset;
        }
        return written;
    }

    /* Pipe write */
    if (f->type == FD_TYPE_PIPE) {
        pipe_t *p = &pipe_table[f->pipe_idx];
        if (count == 0)
            return 0;

        uint32_t written = 0;
        while (written < count) {
            /* If no readers remain, signal broken pipe */
            if (p->readers == 0) {
                task_send_signal(get_current_task(), SIGPIPE);
                return (written > 0) ? (int32_t)written : -1;
            }

            if (p->count < PIPE_BUF_SIZE) {
                /* Write as much as we can */
                uint32_t space = PIPE_BUF_SIZE - p->count;
                uint32_t chunk = count - written;
                if (chunk > space) chunk = space;

                for (uint32_t i = 0; i < chunk; i++) {
                    p->buf[p->write_pos] = (uint8_t)buf[written + i];
                    p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
                }
                p->count += chunk;
                written += chunk;

                /* Wake up any blocked readers */
                /* (readers block via schedule loop, they'll be woken by the scheduler) */
            } else {
                /* Buffer full — block and retry (scheduler will reschedule us) */
                task_t *me = get_current_task();
                if (me) {
                    me->state = TASK_INTERRUPTIBLE;
                    schedule();
                } else {
                    asm volatile("sti; hlt");
                }
            }
        }
        return (int32_t)written;
    }

    return -1;
}

/* SYS_READ (2): read bytes from a file descriptor.
 *   ebx = fd
 *   ecx = pointer to buffer
 *   edx = max number of bytes
 *   Returns: number of bytes read, or -1 on error. */
static int32_t sys_read(struct isr_regs *regs) {
    uint32_t fd   = regs->ebx;
    char    *buf  = (char *)regs->ecx;
    uint32_t count = regs->edx;

    if (buf == NULL)
        return -1;
    if (count == 0)
        return 0;

    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES)
        return -1;

    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED) || !(f->flags & FD_FLAG_READABLE))
        return -1;

    /* stdin → line-buffered TTY input */
    if (f->type == FD_TYPE_STDIN || f->type == FD_TYPE_DEVTTY) {
        extern uint8_t get_key(void);

        /* If cooked data is already available, return it immediately */
        if (tty_ldisc.cooked_count > 0)
            return tty_read_cooked(buf, count);

        /* If EOF was flagged (Ctrl+D on empty line), clear and return 0 */
        if (tty_ldisc.eof_pending) {
            tty_ldisc.eof_pending = 0;
            return 0;
        }

        /* Line editing loop: accumulate until Enter or Ctrl+D */
        while (1) {
            uint8_t ch = get_key();

            if (ch == '\r' || ch == '\n') {
                /* Echo newline */
                terminal_write("\n", 1);
                /* Append \n to edit buffer and flush */
                if (tty_ldisc.edit_len < TTY_LINE_SIZE)
                    tty_ldisc.edit_buf[tty_ldisc.edit_len++] = '\n';
                tty_flush_edit();
                return tty_read_cooked(buf, count);
            } else if (ch == 0x04) {
                /* Ctrl+D (EOT) */
                if (tty_ldisc.edit_len == 0) {
                    /* EOF on empty line */
                    return 0;
                } else {
                    /* Flush current buffer without newline */
                    tty_flush_edit();
                    return tty_read_cooked(buf, count);
                }
            } else if (ch == 0x08 || ch == 0x7F) {
                /* Backspace / DEL */
                if (tty_ldisc.edit_len > 0) {
                    tty_ldisc.edit_len--;
                    /* Echo: BS, space, BS to erase character on screen */
                    terminal_write("\b \b", 3);
                }
            } else if (ch == 0x03) {
                /* Ctrl+C — discard current line (SIGINT already sent by IRQ) */
                tty_ldisc.edit_len = 0;
                terminal_write("^C\n", 3);
                /* Return -1 to indicate interrupted */
                return -1;
            } else if (ch == 0x15) {
                /* Ctrl+U — kill line */
                while (tty_ldisc.edit_len > 0) {
                    tty_ldisc.edit_len--;
                    terminal_write("\b \b", 3);
                }
            } else if (ch >= 0x20 && ch < 0x7F) {
                /* Printable character */
                if (tty_ldisc.edit_len < TTY_LINE_SIZE - 1) {
                    tty_ldisc.edit_buf[tty_ldisc.edit_len++] = (char)ch;
                    /* Echo */
                    terminal_write((const char *)&ch, 1);
                }
            } else if (ch == '\t') {
                /* Tab — insert as space(s) or literal tab */
                if (tty_ldisc.edit_len < TTY_LINE_SIZE - 1) {
                    tty_ldisc.edit_buf[tty_ldisc.edit_len++] = '\t';
                    terminal_write("\t", 1);
                }
            }
            /* Other control characters are silently ignored */
        }
    }

    /* /dev/null → always EOF */
    if (f->type == FD_TYPE_DEVNULL)
        return 0;

    /* Regular file read */
    if (f->type == FD_TYPE_FILE) {
        int n = ext2_read_file(f->ino, buf, f->offset, count);
        if (n > 0)
            f->offset += (uint32_t)n;
        return n;
    }

    /* Pipe read */
    if (f->type == FD_TYPE_PIPE) {
        pipe_t *p = &pipe_table[f->pipe_idx];

        /* Block until data is available or all writers are gone */
        while (p->count == 0) {
            if (p->writers == 0)
                return 0;  /* EOF: no writers left */
            /* Block until data arrives */
            task_t *me = get_current_task();
            if (me) {
                me->state = TASK_INTERRUPTIBLE;
                schedule();
            } else {
                asm volatile("sti; hlt");
            }
        }

        /* Read as much as available, up to count */
        uint32_t avail = p->count;
        uint32_t to_read = (count < avail) ? count : avail;

        for (uint32_t i = 0; i < to_read; i++) {
            buf[i] = (char)p->buf[p->read_pos];
            p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
        }
        p->count -= to_read;

        return (int32_t)to_read;
    }

    return -1;
}

/* SYS_GETPID (3): return the current task's pid.
 *   No arguments.
 *   Returns: pid. */
static int32_t sys_getpid(struct isr_regs *regs) {
    (void)regs;
    task_t *t = get_current_task();
    if (t)
        return (int32_t)t->pid;
    return -1;
}

/* Paging helpers (arch/i386/paging.c) */
extern void paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
extern uint32_t *paging_get_page_dir(void);

#define PTE_PRESENT  0x001
#define PTE_WRITABLE 0x002
#define PTE_USER     0x004
#define PDE_USER     0x004
#define SYS_PAGE_SIZE 4096

/* Stack region boundary (must match elf.c USER_STACK_BASE) */
#define USER_STACK_BASE 0xBFEFF000u

/* SYS_BRK (4): adjust the program break.
 *   ebx = new_brk  (desired break address; 0 = query current break)
 *   Returns: current program break on success.
 *            On failure, returns the unchanged old break. */
static int32_t sys_brk(struct isr_regs *regs) {
    uint32_t new_brk = regs->ebx;
    task_t *t = get_current_task();
    if (!t)
        return -1;

    /* Query only */
    if (new_brk == 0)
        return (int32_t)t->brk_current;

    /* Cannot shrink below initial break */
    if (new_brk < t->brk_start)
        return (int32_t)t->brk_current;

    /* Don't allow the heap to grow into the user stack region.
     * Also guard against overflow: if new_brk is close to 0xFFFFFFFF,
     * the page rounding below can wrap to 0. */
    if (new_brk >= USER_STACK_BASE || new_brk < t->brk_start)
        return (int32_t)t->brk_current;

    uint32_t old_page = (t->brk_current + SYS_PAGE_SIZE - 1) & ~(SYS_PAGE_SIZE - 1);
    uint32_t new_page = (new_brk + SYS_PAGE_SIZE - 1) & ~(SYS_PAGE_SIZE - 1);
    /* Overflow check */
    if (new_page < new_brk)
        return (int32_t)t->brk_current;

    /* Grow: allocate and map new pages */
    for (uint32_t addr = old_page; addr < new_page; addr += SYS_PAGE_SIZE) {
        if (elf_map_user_page(addr) == NULL)
            return (int32_t)t->brk_current;  /* out of memory */
    }

    t->brk_current = new_brk;
    return (int32_t)new_brk;
}

/* ------------------------------------------------------------------ */
/*  File descriptor helpers                                            */
/* ------------------------------------------------------------------ */

/* Find the lowest unused fd slot. Returns -1 if table is full. */
static int alloc_fd(task_t *t) {
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        if (!(t->fd_table[i].flags & FD_FLAG_USED))
            return i;
    }
    return -1;
}

/* Use the shared path resolver from fs.c */
#define resolve_path fs_resolve_path

/* ------------------------------------------------------------------ */
/*  SYS_OPEN / SYS_CLOSE / SYS_LSEEK                                  */
/* ------------------------------------------------------------------ */

/* SYS_OPEN (5): open a file by path.
 *   ebx = pointer to path string
 *   ecx = flags (O_RDONLY, O_WRONLY, O_RDWR, O_CREAT, O_TRUNC)
 *   Returns: file descriptor number, or -1 on error. */
static int32_t sys_open(struct isr_regs *regs) {
    const char *path = (const char *)regs->ebx;
    uint32_t flags   = regs->ecx;

    if (!path)
        return -1;

    task_t *t = get_current_task();
    if (!t)
        return -1;

    int fd = alloc_fd(t);
    if (fd < 0)
        return -1;  /* no free descriptors */

    /* ---- Device file handling ---- */
    /* /dev/null: reads return EOF, writes are discarded */
    if (kstrcmp(path, "/dev/null") == 0) {
        fd_entry_t *f = &t->fd_table[fd];
        f->type   = FD_TYPE_DEVNULL;
        f->flags  = FD_FLAG_USED | FD_FLAG_READABLE | FD_FLAG_WRITABLE;
        f->ino    = 0;
        f->offset = 0;
        f->file_size = 0;
        f->pipe_idx  = 0;
        return fd;
    }

    /* /dev/tty, /dev/console: reads from TTY, writes to terminal */
    if (kstrcmp(path, "/dev/tty") == 0 ||
        kstrcmp(path, "/dev/console") == 0) {
        fd_entry_t *f = &t->fd_table[fd];
        f->type   = FD_TYPE_DEVTTY;
        f->flags  = FD_FLAG_USED | FD_FLAG_READABLE | FD_FLAG_WRITABLE;
        f->ino    = 0;
        f->offset = 0;
        f->file_size = 0;
        f->pipe_idx  = 0;
        return fd;
    }

    /* /dev/stdin, /dev/stdout, /dev/stderr — alias to fd 0/1/2 */
    if (kstrcmp(path, "/dev/stdin") == 0) {
        t->fd_table[fd] = t->fd_table[0];
        return fd;
    }
    if (kstrcmp(path, "/dev/stdout") == 0) {
        t->fd_table[fd] = t->fd_table[1];
        return fd;
    }
    if (kstrcmp(path, "/dev/stderr") == 0) {
        t->fd_table[fd] = t->fd_table[2];
        return fd;
    }

    uint32_t ino = resolve_path(path);

    if (ino == 0 && (flags & O_CREAT)) {
        /* File doesn't exist; create it.
         * Find the parent directory and the filename component. */
        const char *last_slash = NULL;
        for (const char *p = path; *p; p++)
            if (*p == '/')
                last_slash = p;

        uint32_t parent_ino;
        const char *filename;
        if (last_slash) {
            /* Temporarily resolve the parent directory */
            char parent_path[256];
            int plen = (int)(last_slash - path);
            if (plen == 0) {
                parent_ino = EXT2_ROOT_INO;
            } else {
                if (plen > 255) plen = 255;
                memcpy(parent_path, path, plen);
                parent_path[plen] = '\0';
                parent_ino = resolve_path(parent_path);
            }
            filename = last_slash + 1;
        } else {
            parent_ino = cwd_ino;
            filename = path;
        }

        if (parent_ino == 0 || filename[0] == '\0')
            return -1;

        ino = ext2_create(parent_ino, filename,
                          EXT2_S_IFREG | EXT2_S_IRWXU | EXT2_S_IRWXG | EXT2_S_IRWXO);
        if (ino == 0)
            return -1;  /* creation failed */
    }

    if (ino == 0)
        return -1;  /* file not found and O_CREAT not set */

    /* Read the inode to get file size and verify it's a regular file or directory */
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;

    uint32_t ftype = inode.i_mode & 0xF000;
    if (ftype != EXT2_S_IFREG && ftype != EXT2_S_IFDIR)
        return -1;  /* not a regular file or directory */

    /* If O_DIRECTORY is set, verify it's actually a directory */
    if ((flags & O_DIRECTORY) && ftype != EXT2_S_IFDIR)
        return -20;  /* -ENOTDIR */

    /* Don't allow writing to directories */
    if (ftype == EXT2_S_IFDIR && (flags & 0x03) != O_RDONLY)
        return -21;  /* -EISDIR */

    /* Permission check */
    {
        int need = 0;
        uint32_t access_mode = flags & 0x03;
        if (access_mode == O_RDONLY || access_mode == O_RDWR)
            need |= R_OK;
        if (access_mode == O_WRONLY || access_mode == O_RDWR)
            need |= W_OK;
        if (!check_permission(t->euid, t->egid, inode.i_uid, inode.i_gid,
                              inode.i_mode & 0xFFF, need))
            return -1;  /* permission denied */
    }

    if (flags & O_TRUNC) {
        ext2_truncate(ino);
        inode.i_size = 0;
    }

    /* Set up the fd entry */
    fd_entry_t *f = &t->fd_table[fd];
    f->type      = FD_TYPE_FILE;
    f->ino       = ino;
    f->offset    = 0;
    f->file_size = inode.i_size;

    uint32_t access = flags & 0x03;  /* O_RDONLY=0, O_WRONLY=1, O_RDWR=2 */
    f->flags = FD_FLAG_USED;
    if (access == O_RDONLY || access == O_RDWR)
        f->flags |= FD_FLAG_READABLE;
    if (access == O_WRONLY || access == O_RDWR)
        f->flags |= FD_FLAG_WRITABLE;

    return fd;
}

/* SYS_CLOSE (6): close a file descriptor.
 *   ebx = fd
 *   Returns: 0 on success, -1 on error. */
static int32_t sys_close(struct isr_regs *regs) {
    uint32_t fd = regs->ebx;

    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES)
        return -1;

    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED))
        return -1;

    /* Handle pipe reference counting */
    if (f->type == FD_TYPE_PIPE && f->pipe_idx < MAX_PIPES) {
        pipe_t *p = &pipe_table[f->pipe_idx];
        if (f->flags & FD_FLAG_READABLE)
            p->readers--;
        if (f->flags & FD_FLAG_WRITABLE)
            p->writers--;
        pipe_maybe_free(f->pipe_idx);
    }

    f->type      = FD_TYPE_NONE;
    f->flags     = 0;
    f->ino       = 0;
    f->offset    = 0;
    f->file_size = 0;
    f->pipe_idx  = 0;

    return 0;
}

/* SYS_LSEEK (7): reposition the file offset.
 *   ebx = fd
 *   ecx = offset
 *   edx = whence (SEEK_SET=0, SEEK_CUR=1, SEEK_END=2)
 *   Returns: new offset on success, -1 on error. */
static int32_t sys_lseek(struct isr_regs *regs) {
    uint32_t fd     = regs->ebx;
    int32_t  offset = (int32_t)regs->ecx;
    uint32_t whence = regs->edx;

    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES)
        return -1;

    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED) || f->type != FD_TYPE_FILE)
        return -1;

    int32_t new_offset;
    switch (whence) {
    case SEEK_SET:
        new_offset = offset;
        break;
    case SEEK_CUR:
        new_offset = (int32_t)f->offset + offset;
        break;
    case SEEK_END:
        new_offset = (int32_t)f->file_size + offset;
        break;
    default:
        return -1;
    }

    if (new_offset < 0)
        return -1;

    f->offset = (uint32_t)new_offset;
    return new_offset;
}

/* ------------------------------------------------------------------ */
/*  Syscall table                                                      */
/* ------------------------------------------------------------------ */

/* Framebuffer info (from arch/i386/framebuffer.c) */
typedef struct {
    uint8_t  *address;
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;
    uint8_t   bpp;
    uint8_t   red_pos, red_size;
    uint8_t   green_pos, green_size;
    uint8_t   blue_pos, blue_size;
} framebuffer_info_t;
extern framebuffer_info_t *fb_get_info(void);
extern uint8_t *fb_get_shadow(void);
extern void fb_flush_full(void);

/* SB16 audio driver (from arch/i386/sb16.c) */
extern uint32_t sb16_write(const uint8_t *buf, uint32_t len);
extern uint32_t sb16_avail(void);
extern void sb16_start(uint32_t sample_rate);

/* Timer (from arch/i386/timer.c) */
extern uint32_t timer_get_ticks(void);

/* Struct returned to user-space by SYS_FBMAP */
typedef struct {
    uint32_t address;    /* virtual address of the mapped framebuffer */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint8_t  red_pos, red_size;
    uint8_t  green_pos, green_size;
    uint8_t  blue_pos, blue_size;
} __attribute__((packed)) user_fbinfo_t;

/* SYS_FBMAP (8): map the framebuffer into user-accessible memory.
 *   ebx = pointer to user_fbinfo_t struct (filled on return)
 *   Returns: virtual address of mapped shadow framebuffer, or 0 on error.
 *
 *   Maps the kernel's shadow framebuffer (cached RAM) into user space
 *   instead of the raw MMIO address, for much faster pixel writes.
 *   User must call SYS_FB_FLUSH to push changes to the display. */
static int32_t sys_fbmap(struct isr_regs *regs) {
    user_fbinfo_t *uinfo = (user_fbinfo_t *)regs->ebx;
    framebuffer_info_t *fb = fb_get_info();
    if (!fb || !fb->address)
        return 0;

    uint8_t *shadow = fb_get_shadow();
    uint32_t shadow_addr = (uint32_t)shadow;
    uint32_t fb_size  = fb->pitch * fb->height;
    uint32_t start = shadow_addr & ~(SYS_PAGE_SIZE - 1);
    uint32_t end   = (shadow_addr + fb_size + SYS_PAGE_SIZE - 1) & ~(SYS_PAGE_SIZE - 1);

    /* Mark shadow buffer pages as user-accessible */
    uint32_t *kernel_pd = paging_get_page_dir();
    task_t *cur = get_current_task();
    uint32_t *user_pd = (cur && cur->page_dir) ? cur->page_dir : kernel_pd;

    for (uint32_t addr = start; addr < end; addr += SYS_PAGE_SIZE) {
        paging_map_page(addr, addr, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        kernel_pd[addr >> 22] |= PDE_USER;
        if (user_pd != kernel_pd)
            user_pd[addr >> 22] |= PDE_USER;
    }

    /* Fill the info struct if provided */
    if (uinfo) {
        uinfo->address   = shadow_addr;
        uinfo->width     = fb->width;
        uinfo->height    = fb->height;
        uinfo->pitch     = fb->pitch;
        uinfo->bpp       = fb->bpp;
        uinfo->red_pos   = fb->red_pos;
        uinfo->red_size  = fb->red_size;
        uinfo->green_pos = fb->green_pos;
        uinfo->green_size= fb->green_size;
        uinfo->blue_pos  = fb->blue_pos;
        uinfo->blue_size = fb->blue_size;
    }

    return (int32_t)shadow_addr;
}

/* SYS_FB_FLUSH (38): flush shadow framebuffer to display.
 *   No arguments.
 *   Returns: 0. */
static int32_t sys_fb_flush(struct isr_regs *regs) {
    (void)regs;
    fb_flush_full();
    return 0;
}

/* SYS_AUDIO_WRITE (39): write PCM samples to audio ring buffer.
 *   ebx = pointer to sample data (unsigned 8-bit PCM)
 *   ecx = number of bytes
 *   Returns: number of bytes actually written. */
static int32_t sys_audio_write(struct isr_regs *regs) {
    const uint8_t *buf = (const uint8_t *)regs->ebx;
    uint32_t len = regs->ecx;
    if (!buf)
        return -1;
    return (int32_t)sb16_write(buf, len);
}

/* SYS_AUDIO_AVAIL (40): query free space in audio ring buffer.
 *   No arguments.
 *   Returns: number of bytes of free space. */
static int32_t sys_audio_avail(struct isr_regs *regs) {
    (void)regs;
    return (int32_t)sb16_avail();
}

/* SYS_AUDIO_START (41): start SB16 playback.
 *   ebx = sample rate in Hz
 *   Returns: 0. */
static int32_t sys_audio_start(struct isr_regs *regs) {
    uint32_t rate = regs->ebx;
    sb16_start(rate);
    return 0;
}

/* SYS_GETTICKS (9): return PIT tick count since boot.
 *   No arguments.
 *   Returns: tick count (1000 Hz, so ticks = milliseconds). */
static int32_t sys_getticks(struct isr_regs *regs) {
    (void)regs;
    return (int32_t)timer_get_ticks();
}

/* SYS_USLEEP (10): sleep for approximately N milliseconds.
 *   ebx = milliseconds to sleep
 *   Returns: 0. */
static int32_t sys_usleep(struct isr_regs *regs) {
    uint32_t ms = regs->ebx;
    /* Timer runs at 1000 Hz → 1 ms per tick */
    uint32_t wait_ticks = ms;
    uint32_t start = timer_get_ticks();
    while (timer_get_ticks() - start < wait_ticks) {
        /* Yield the CPU: set interruptible so scheduler picks other tasks */
        task_t *me = get_current_task();
        if (me) {
            me->state = TASK_INTERRUPTIBLE;
            schedule();
        } else {
            asm volatile("sti; hlt");
        }
    }
    return 0;
}

/* SYS_WRITE_N (11): write N bytes to terminal from user buffer.
 *   ebx = pointer to buffer
 *   ecx = number of bytes
 *   Returns: number of bytes written. */
static int32_t sys_write_n(struct isr_regs *regs) {
    const char *buf = (const char *)regs->ebx;
    uint32_t count  = regs->ecx;
    if (!buf)
        return -1;
    terminal_write(buf, (size_t)count);
    return (int32_t)count;
}

/* SYS_POLL_KEY (12): non-blocking keyboard event poll.
 *   ebx = pointer to key_event_t in user space
 *   Returns: 1 if an event was written, 0 if queue empty. */
static int32_t sys_poll_key(struct isr_regs *regs) {
    key_event_t *out = (key_event_t *)regs->ebx;
    if (!out)
        return -1;
    return keyboard_poll_event(out) ? 1 : 0;
}

/* SYS_WAIT_KEY (13): blocking keyboard event wait.
 *   ebx = pointer to key_event_t in user space
 *   Returns: 1 always (event written). */
static int32_t sys_wait_key(struct isr_regs *regs) {
    key_event_t *out = (key_event_t *)regs->ebx;
    if (!out)
        return -1;
    while (!keyboard_poll_event(out))
        asm volatile("sti; hlt");
    return 1;
}

/* SYS_POLL_MOUSE (513): non-blocking mouse event poll.
 *   ebx = pointer to mouse_event_t in user space
 *   Returns: 1 if an event was written, 0 if queue empty, -1 on error. */
static int32_t sys_poll_mouse(struct isr_regs *regs) {
    mouse_event_t *out = (mouse_event_t *)regs->ebx;
    if (!out)
        return -1;
    return mouse_poll_event(out) ? 1 : 0;
}

/* SYS_GET_MOUSE_POS (514): get absolute mouse cursor position.
 *   ebx = pointer to int32_t[2] in user space (x, y)
 *   Returns: button state, or -1 on error. */
static int32_t sys_get_mouse_pos(struct isr_regs *regs) {
    int32_t *out = (int32_t *)regs->ebx;
    if (!out)
        return -1;
    mouse_get_position(&out[0], &out[1]);
    return (int32_t)mouse_get_buttons();
}

/* SYS_GET_WINSIZE (14): return terminal dimensions.
 *   ebx = pointer to uint32_t[2] in user space (rows, cols)
 *   Returns: 0 on success, -1 on error. */
static int32_t sys_get_winsize(struct isr_regs *regs) {
    uint32_t *out = (uint32_t *)regs->ebx;
    if (!out)
        return -1;
    out[0] = (uint32_t)terminal_get_rows();
    out[1] = (uint32_t)terminal_get_cols();
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Stat structure returned to user-space                              */
/* ------------------------------------------------------------------ */

typedef struct {
    uint32_t st_ino;
    uint16_t st_mode;
    uint16_t st_nlink;
    uint32_t st_size;
    uint32_t st_atime;
    uint32_t st_mtime;
    uint32_t st_ctime;
    uint32_t st_blksize;
    uint32_t st_blocks;
} moss_stat_t;

static void inode_to_stat(uint32_t ino, const ext2_inode_t *in, moss_stat_t *st) {
    st->st_ino     = ino;
    st->st_mode    = in->i_mode;
    st->st_nlink   = in->i_links_count;
    st->st_size    = in->i_size;
    st->st_atime   = in->i_atime;
    st->st_mtime   = in->i_mtime;
    st->st_ctime   = in->i_ctime;
    st->st_blksize = 1024;
    st->st_blocks  = in->i_blocks;
}

/* SYS_STAT (15): stat a file by path.
 *   ebx = pointer to path string
 *   ecx = pointer to moss_stat_t struct
 *   Returns: 0 on success, -1 on error. */
static int32_t sys_stat(struct isr_regs *regs) {
    const char *path = (const char *)regs->ebx;
    moss_stat_t *st  = (moss_stat_t *)regs->ecx;
    if (!path || !st)
        return -1;

    uint32_t ino = resolve_path(path);
    if (ino == 0)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;

    inode_to_stat(ino, &inode, st);
    return 0;
}

/* SYS_FSTAT (16): stat an open file descriptor.
 *   ebx = fd
 *   ecx = pointer to moss_stat_t struct
 *   Returns: 0 on success, -1 on error. */
static int32_t sys_fstat(struct isr_regs *regs) {
    uint32_t fd     = regs->ebx;
    moss_stat_t *st = (moss_stat_t *)regs->ecx;
    if (!st)
        return -1;

    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES)
        return -1;

    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED) || f->type != FD_TYPE_FILE)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(f->ino, &inode) != 0)
        return -1;

    inode_to_stat(f->ino, &inode, st);
    return 0;
}

/* SYS_UNLINK (17): remove a file by path.
 *   ebx = pointer to path string
 *   Returns: 0 on success, -1 on error. */
static int32_t sys_unlink(struct isr_regs *regs) {
    const char *path = (const char *)regs->ebx;
    if (!path)
        return -1;

    /* Split into parent dir + filename */
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            last_slash = p;

    uint32_t parent_ino;
    const char *filename;
    if (last_slash) {
        char parent_path[256];
        int plen = (int)(last_slash - path);
        if (plen == 0) {
            parent_ino = EXT2_ROOT_INO;
        } else {
            if (plen > 255) plen = 255;
            memcpy(parent_path, path, plen);
            parent_path[plen] = '\0';
            parent_ino = resolve_path(parent_path);
        }
        filename = last_slash + 1;
    } else {
        parent_ino = cwd_ino;
        filename = path;
    }

    if (parent_ino == 0 || filename[0] == '\0')
        return -1;

    /* Check write permission on parent directory */
    {
        task_t *t = get_current_task();
        if (t) {
            ext2_inode_t dir_inode;
            if (ext2_read_inode(parent_ino, &dir_inode) == 0) {
                if (!check_permission(t->euid, t->egid, dir_inode.i_uid, dir_inode.i_gid,
                                      dir_inode.i_mode & 0xFFF, W_OK))
                    return -1;
            }
        }
    }

    return ext2_remove(parent_ino, filename);
}

/* Helper: build the absolute path for the current cwd_ino.
 * Not used — we track cwd_path directly. */

/* SYS_GETCWD (18): get current working directory path.
 *   ebx = pointer to buffer
 *   ecx = buffer size
 *   Returns: pointer to buffer on success, 0 on error. */
static int32_t sys_getcwd(struct isr_regs *regs) {
    char *buf     = (char *)regs->ebx;
    uint32_t size = regs->ecx;
    if (!buf || size == 0)
        return 0;

    extern char cwd_path[256];
    size_t len = strlen(cwd_path);
    if (len + 1 > size)
        return 0;
    memcpy(buf, cwd_path, len + 1);
    return (int32_t)(uint32_t)buf;
}

/* SYS_CHDIR (19): change the current working directory.
 *   ebx = pointer to path string
 *   Returns: 0 on success, -1 on error. */
static int32_t sys_chdir(struct isr_regs *regs) {
    const char *path = (const char *)regs->ebx;
    if (!path)
        return -1;

    uint32_t ino = resolve_path(path);
    if (ino == 0)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;

    if ((inode.i_mode & 0xF000) != EXT2_S_IFDIR)
        return -1;

    cwd_ino = ino;

    /* Update cwd_path using only memcpy/strlen/memcmp */
    extern char cwd_path[256];
    size_t plen = strlen(path);

    if (path[0] == '/') {
        /* Absolute path — copy directly */
        size_t copy = plen < 255 ? plen : 255;
        memcpy(cwd_path, path, copy);
        cwd_path[copy] = '\0';
    } else if (plen == 2 && path[0] == '.' && path[1] == '.') {
        /* Go up one level: find last '/' and truncate */
        size_t cur_len = strlen(cwd_path);
        /* Find last '/' that isn't the leading one */
        int last_slash = -1;
        for (int i = (int)cur_len - 1; i > 0; i--) {
            if (cwd_path[i] == '/') { last_slash = i; break; }
        }
        if (last_slash > 0)
            cwd_path[last_slash] = '\0';
        else {
            cwd_path[0] = '/';
            cwd_path[1] = '\0';
        }
    } else if (!(plen == 1 && path[0] == '.')) {
        /* Relative path — append */
        size_t cur_len = strlen(cwd_path);
        if (cur_len > 1 && cur_len < 254) {
            cwd_path[cur_len] = '/';
            cur_len++;
        }
        size_t copy = plen;
        if (cur_len + copy > 255) copy = 255 - cur_len;
        memcpy(cwd_path + cur_len, path, copy);
        cwd_path[cur_len + copy] = '\0';
    }
    return 0;
}

/* SYS_GETTIME (20): get seconds since boot.
 *   Returns: seconds (ticks / 1000). */
static int32_t sys_gettime(struct isr_regs *regs) {
    (void)regs;
    return (int32_t)(timer_get_ticks() / 1000);
}

/* SYS_DUP (21): duplicate a file descriptor.
 *   ebx = fd to duplicate
 *   Returns: new fd, or -1 on error. */
static int32_t sys_dup(struct isr_regs *regs) {
    uint32_t old_fd = regs->ebx;

    task_t *t = get_current_task();
    if (!t || old_fd >= MAX_OPEN_FILES)
        return -1;

    fd_entry_t *of = &t->fd_table[old_fd];
    if (!(of->flags & FD_FLAG_USED))
        return -1;

    int new_fd = alloc_fd(t);
    if (new_fd < 0)
        return -1;

    t->fd_table[new_fd] = *of;

    /* Increment pipe reference count */
    if (of->type == FD_TYPE_PIPE) {
        pipe_t *p = &pipe_table[of->pipe_idx];
        if (of->flags & FD_FLAG_READABLE) p->readers++;
        if (of->flags & FD_FLAG_WRITABLE) p->writers++;
    }

    return new_fd;
}

/* SYS_DUP2 (22): duplicate fd to a specific target.
 *   ebx = old fd
 *   ecx = new fd
 *   Returns: new fd, or -1 on error. */
static int32_t sys_dup2(struct isr_regs *regs) {
    uint32_t old_fd = regs->ebx;
    uint32_t new_fd = regs->ecx;

    task_t *t = get_current_task();
    if (!t || old_fd >= MAX_OPEN_FILES || new_fd >= MAX_OPEN_FILES)
        return -1;

    fd_entry_t *of = &t->fd_table[old_fd];
    if (!(of->flags & FD_FLAG_USED))
        return -1;

    if (old_fd == new_fd)
        return (int32_t)new_fd;

    /* Close new_fd if it's open */
    fd_entry_t *nf = &t->fd_table[new_fd];
    if (nf->flags & FD_FLAG_USED) {
        /* Decrement pipe refcount on the old fd being replaced */
        if (nf->type == FD_TYPE_PIPE) {
            pipe_t *p = &pipe_table[nf->pipe_idx];
            if (nf->flags & FD_FLAG_READABLE) p->readers--;
            if (nf->flags & FD_FLAG_WRITABLE) p->writers--;
            pipe_maybe_free(nf->pipe_idx);
        }
        nf->type = FD_TYPE_NONE;
        nf->flags = 0;
    }

    t->fd_table[new_fd] = *of;

    /* Increment pipe reference count for the new dup */
    if (of->type == FD_TYPE_PIPE) {
        pipe_t *p = &pipe_table[of->pipe_idx];
        if (of->flags & FD_FLAG_READABLE) p->readers++;
        if (of->flags & FD_FLAG_WRITABLE) p->writers++;
    }

    return (int32_t)new_fd;
}

/* ------------------------------------------------------------------ */
/*  Fork helpers                                                       */
/* ------------------------------------------------------------------ */

/* Paging helpers for fork */
extern uint32_t *paging_create_user_directory(void);
extern void paging_free_user_directory(uint32_t *pd);
extern void paging_map_page_in(uint32_t *pd, uint32_t virt, uint32_t phys,
                               uint32_t flags);
extern uint32_t paging_get_physical_in(uint32_t *pd, uint32_t virt);

/* ELF per-process page management */
extern int elf_alloc_page_slot(void);
extern void elf_free_page_slot(int slot);
extern int elf_get_slot_page_count(int slot);
extern uint32_t elf_get_slot_vaddr(int slot, int index);
extern uint32_t elf_get_slot_paddr(int slot, int index);
extern void *elf_map_user_page_in(uint32_t vaddr, uint32_t *page_dir, int slot);

/*
 * Entry point for fork child tasks.
 * Restores saved user-mode state and does iret to ring 3 with eax=0.
 *
 * IMPORTANT: All struct field reads must happen BEFORE the asm block,
 * because the asm clobbers GP registers that the compiler may use to
 * dereference the task_t pointer.  We read everything into locals first.
 */
static void fork_child_entry(void) {
    task_t *self = get_current_task();

    if (!self) {
        exit_task(-1);
        __builtin_unreachable();
    }

    /* Read ALL values from the struct into locals BEFORE entering asm.
     * The asm clobbers ebp/edi/esi/etc, so any "m" constraint that relied
     * on those registers to reach `self->field` would read garbage. */
    uint32_t f_eip    = self->fork_eip;
    uint32_t f_esp    = self->fork_esp;
    uint32_t f_eflags = self->fork_eflags;
    uint32_t f_ebx    = self->fork_ebx;
    uint32_t f_ecx    = self->fork_ecx;
    uint32_t f_edx    = self->fork_edx;
    uint32_t f_esi    = self->fork_esi;
    uint32_t f_edi    = self->fork_edi;
    uint32_t f_ebp    = self->fork_ebp;

    /* TLS: if parent had TLS, restore the GDT entry and use its GS */
    uint16_t f_gs_sel = 0x23;   /* default: user data segment */
    if (self->tls_gs) {
        extern int gdt_set_tls(int entry_number, uint32_t base, uint32_t limit);
        gdt_set_tls(self->tls_entry, self->tls_base, self->tls_limit);
        f_gs_sel = self->tls_gs;
    }

    /* Validate the saved user EIP looks sane (in user-space range) */
    if (f_eip < 0x08048000u || f_eip >= 0xC0000000u) {
        exit_task(-1);
        __builtin_unreachable();
    }
    /* Validate user ESP is in the user stack region */
    if (f_esp < 0x08000000u || f_esp >= 0xC0000000u) {
        exit_task(-1);
        __builtin_unreachable();
    }

    asm volatile(
        "cli\n\t"
        /* Load user data segment into DS/ES/FS */
        "movw $0x23, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        /* Load GS with TLS selector (or user data if no TLS) */
        "movw %[gs_sel], %%ax\n\t"
        "movw %%ax, %%gs\n\t"
        /* Build the iret frame on the kernel stack */
        "pushl $0x23\n\t"        /* SS  = user data */
        "pushl %[esp]\n\t"       /* ESP = saved user esp */
        "pushl %[eflags]\n\t"    /* EFLAGS */
        "pushl $0x1B\n\t"        /* CS  = user code */
        "pushl %[eip]\n\t"       /* EIP = saved user eip */
        /* Now restore GP registers (safe — iret frame already pushed) */
        "movl %[ebp], %%ebp\n\t"
        "movl %[edi], %%edi\n\t"
        "movl %[esi], %%esi\n\t"
        "movl %[edx], %%edx\n\t"
        "movl %[ecx], %%ecx\n\t"
        "movl %[ebx], %%ebx\n\t"
        /* eax = 0 → fork() returns 0 in the child */
        "xorl %%eax, %%eax\n\t"
        "iret\n\t"
        :
        : [eip]    "r" (f_eip),
          [esp]    "r" (f_esp),
          [eflags] "r" (f_eflags),
          [gs_sel] "m" (f_gs_sel),
          [ebp]    "m" (f_ebp),
          [edi]    "m" (f_edi),
          [esi]    "m" (f_esi),
          [edx]    "m" (f_edx),
          [ecx]    "m" (f_ecx),
          [ebx]    "m" (f_ebx)
        : "eax", "memory"
    );
    __builtin_unreachable();
}

/* SYS_FORK (23): create a copy of the current process.
 *   No arguments.
 *   Returns: child PID to parent, 0 to child, -1 on error. */
static int32_t sys_fork(struct isr_regs *regs) {
    task_t *parent = get_current_task();
    if (!parent)
        return -1;
    /* Parent must have a page directory and user pages slot */
    if (!parent->page_dir)
        return -1;
    if (parent->user_pages_slot < 0)
        return -1;

    /* Validate user EIP */
    if (regs->eip < 0x08048000u || regs->eip >= 0xC0000000u) {
        return -1;
    }
    /* Validate user ESP */
    if (regs->useresp < 0x08000000u || regs->useresp >= 0xC0000000u)
        return -1;

    /* Create a new page directory for the child */
    uint32_t *child_pd = paging_create_user_directory();
    if (!child_pd)
        return -1;

    /* Allocate a user pages tracking slot for the child */
    int child_slot = elf_alloc_page_slot();
    if (child_slot < 0) {
        paging_free_user_directory(child_pd);
        return -1;
    }

    /* Copy all parent user pages to new physical pages in child's PD */
    int parent_slot = parent->user_pages_slot;
    int parent_page_count = elf_get_slot_page_count(parent_slot);

    for (int i = 0; i < parent_page_count; i++) {
        uint32_t vaddr = elf_get_slot_vaddr(parent_slot, i);
        uint32_t parent_paddr = elf_get_slot_paddr(parent_slot, i);

        /* Allocate a new physical page and copy parent's data */
        void *child_page = elf_map_user_page_in(vaddr, child_pd, child_slot);
        if (!child_page) {
            elf_free_page_slot(child_slot);
            paging_free_user_directory(child_pd);
            return -1;
        }

        /* Copy page content from parent */
        memcpy(child_page, (void *)parent_paddr, SYS_PAGE_SIZE);
    }

    /* Create child task */
    task_t child_task = task_create(fork_child_entry, parent->priority);
    if (child_task.esp == 0 && child_task.stack == NULL) {
        elf_free_page_slot(child_slot);
        paging_free_user_directory(child_pd);
        return -1;
    }

    /* Copy parent state to child */
    child_task.page_dir        = child_pd;
    child_task.user_pages_slot = child_slot;
    child_task.parent_pid      = parent->pid;
    child_task.brk_start       = parent->brk_start;
    child_task.brk_current     = parent->brk_current;

    /* Inherit user credentials */
    child_task.uid  = parent->uid;
    child_task.gid  = parent->gid;
    child_task.euid = parent->euid;
    child_task.egid = parent->egid;

    /* Copy fd table */
    memcpy(child_task.fd_table, parent->fd_table, sizeof(parent->fd_table));

    /* Increment pipe reference counts for inherited pipe fds */
    for (int i = 0; i < MAX_OPEN_FILES; i++) {
        fd_entry_t *f = &child_task.fd_table[i];
        if ((f->flags & FD_FLAG_USED) && f->type == FD_TYPE_PIPE) {
            pipe_t *p = &pipe_table[f->pipe_idx];
            if (f->flags & FD_FLAG_READABLE) p->readers++;
            if (f->flags & FD_FLAG_WRITABLE) p->writers++;
        }
    }

    /* Copy per-process cwd */
    child_task.cwd_ino = parent->cwd_ino;
    memcpy(child_task.cwd_path, parent->cwd_path, sizeof(parent->cwd_path));

    /* Inherit signal handlers and blocked mask (POSIX fork semantics) */
    child_task.sig_blocked = parent->sig_blocked;
    for (int i = 0; i < _NSIG; i++)
        child_task.sig_handlers[i] = parent->sig_handlers[i];

    /* Save parent's user-mode registers so child can resume.
     * The isr_regs frame has the user-mode state at the point of the
     * int $0x80 syscall. */
    child_task.fork_eip    = regs->eip;
    child_task.fork_esp    = regs->useresp;
    child_task.fork_eflags = regs->eflags;
    child_task.fork_ebx    = regs->ebx;
    child_task.fork_ecx    = regs->ecx;
    child_task.fork_edx    = regs->edx;
    child_task.fork_esi    = regs->esi;
    child_task.fork_edi    = regs->edi;
    child_task.fork_ebp    = regs->ebp;
    child_task.fork_ds     = regs->ds;
    child_task.fork_gs     = regs->gs;

    /* Copy TLS state so child inherits parent's thread-local storage */
    child_task.tls_gs    = parent->tls_gs;
    child_task.tls_entry = parent->tls_entry;
    child_task.tls_base  = parent->tls_base;
    child_task.tls_limit = parent->tls_limit;

    admit_task(&child_task);

    /* Parent returns child PID */
    return (int32_t)child_task.pid;
}

/* SYS_WAITPID (24): wait for a child process to exit.
 *   ebx = pid (-1 = any child)
 *   ecx = pointer to int for exit status (can be NULL)
 *   Returns: PID of exited child, or -1 on error. */
static int32_t sys_waitpid(struct isr_regs *regs) {
    int32_t  wait_pid = (int32_t)regs->ebx;
    int     *status   = (int *)regs->ecx;

    task_t *parent = get_current_task();
    if (!parent)
        return -1;

    /* Loop until we find an exited child matching the criteria */
    for (;;) {
        if (wait_pid > 0) {
            /* Wait for specific PID */
            task_t *child = find_task_by_pid((uint32_t)wait_pid);
            if (!child || child->parent_pid != parent->pid)
                return -1;  /* no such child */
            if (child->state == TASK_ZOMBIE) {
                int32_t child_pid = (int32_t)child->pid;
                if (status)
                    *status = child->exit_code;
                remove_task(child);
                return child_pid;
            }
        } else {
            /* wait_pid == -1: wait for any child */
            int found_any = 0;
            task_t *zombie = find_child_task(parent->pid, 1, &found_any);
            if (zombie) {
                int32_t child_pid = (int32_t)zombie->pid;
                if (status)
                    *status = zombie->exit_code;
                remove_task(zombie);
                return child_pid;
            }
            if (!found_any)
                return -1;  /* no children at all → ECHILD */
        }

        /* Child exists but hasn't exited yet — block and retry */
        block_task(parent);
    }
}

/* ------------------------------------------------------------------ */
/*  Per-process environment  (simple key=value store)                   */
/* ------------------------------------------------------------------ */

#define ENV_MAX_VARS  64
#define ENV_BUF_SIZE  4096

/* Global environment (shared across all processes for now — simplification).
 * A proper implementation would give each process its own env on fork/exec. */
static char env_buf[ENV_BUF_SIZE];
static int  env_buf_used = 0;
static char *env_vars[ENV_MAX_VARS];  /* pointers into env_buf, "KEY=VALUE" */
static int  env_count = 0;

/* Initialize default environment */
static int env_initialized = 0;
static void env_init(void) {
    if (env_initialized) return;
    env_initialized = 1;
    /* Set up default env vars */
    static const char *defaults[] = {
        "PATH=/bin:/usr/bin",
        "HOME=/root",
        "TERM=linux",
        "SHELL=/bin/bash",
        "USER=root",
        "LOGNAME=root",
        "HOSTNAME=moss",
        "PWD=/",
        "LANG=C",
        "PS1=\\u@\\h:\\w\\$ ",
        NULL
    };
    for (int i = 0; defaults[i]; i++) {
        size_t len = strlen(defaults[i]) + 1;
        if (env_buf_used + (int)len > ENV_BUF_SIZE) break;
        memcpy(env_buf + env_buf_used, defaults[i], len);
        env_vars[env_count] = env_buf + env_buf_used;
        env_buf_used += (int)len;
        env_count++;
    }
}

/* Find env var by key. Returns index or -1. */
static int env_find(const char *key, size_t keylen) {
    for (int i = 0; i < env_count; i++) {
        int match = 1;
        for (size_t j = 0; j < keylen; j++) {
            if (env_vars[i][j] != key[j]) { match = 0; break; }
        }
        if (match && env_vars[i][keylen] == '=')
            return i;
    }
    return -1;
}

/* SYS_EXECVE (25): replace the current process image with a new program.
 *   ebx = pointer to path string
 *   ecx = pointer to argv[] array (NULL-terminated)
 *   edx = pointer to envp[] array (ignored for now)
 *   Returns: does not return on success; -1 on error. */
static int32_t sys_execve(struct isr_regs *regs) {
    const char *path = (const char *)regs->ebx;
    char *const *user_argv = (char *const *)regs->ecx;

    if (!path)
        return -1;

    /* Resolve the path to an inode */
    uint32_t ino = resolve_path(path);
    if (ino == 0)
        return -1;

    /* Verify it's a regular file */
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;
    if ((inode.i_mode & 0xF000) != EXT2_S_IFREG)
        return -1;

    /* Count and copy argv into kernel-space buffers (user memory will
     * be destroyed during exec).  Limit to 64 arguments, 4096 bytes total. */
    int argc = 0;
    char *kargv[64];
    char karg_buf[4096];
    int buf_off = 0;

    if (user_argv) {
        for (int i = 0; i < 64; i++) {
            char *arg = user_argv[i];
            if (!arg)
                break;
            size_t len = strlen(arg);
            if (buf_off + (int)len + 1 > (int)sizeof(karg_buf))
                break;
            memcpy(karg_buf + buf_off, arg, len + 1);
            kargv[i] = karg_buf + buf_off;
            buf_off += (int)len + 1;
            argc++;
        }
    }

    /* If no argv was provided, use the path as argv[0] */
    if (argc == 0) {
        size_t plen = strlen(path);
        if (plen + 1 <= sizeof(karg_buf)) {
            memcpy(karg_buf, path, plen + 1);
            kargv[0] = karg_buf;
            argc = 1;
        }
    }

    /* Ensure argv[0] is the resolved path so shebang scripts get
     * the full path to the script file (not just the basename). */
    if (argc > 0) {
        size_t plen = strlen(path);
        if (buf_off + (int)plen + 1 <= (int)sizeof(karg_buf)) {
            memcpy(karg_buf + buf_off, path, plen + 1);
            kargv[0] = karg_buf + buf_off;
            buf_off += (int)plen + 1;
        }
    }

    /* Copy envp from the kernel environment store */
    env_init();
    char *kenvp[ENV_MAX_VARS];
    int kenvc = 0;
    for (int i = 0; i < env_count && i < ENV_MAX_VARS; i++) {
        kenvp[i] = env_vars[i];
        kenvc++;
    }

    /* elf_exec_replace replaces the address space and jumps to user
     * mode — it never returns on success.  On failure the process
     * is killed (can't recover since old image is already destroyed). */
    elf_exec_replace(ino, argc, kargv, kenvc, kenvp);

    /* Should never reach here */
    return -1;
}

/* SYS_PIPE (26): create a pipe.
 *   ebx = pointer to int[2] in user space (pipefd[0]=read, pipefd[1]=write)
 *   Returns: 0 on success, -1 on error. */
static int32_t sys_pipe(struct isr_regs *regs) {
    int *pipefd = (int *)regs->ebx;
    if (!pipefd)
        return -1;

    task_t *t = get_current_task();
    if (!t)
        return -1;

    /* Allocate a pipe */
    int pidx = pipe_alloc();
    if (pidx < 0)
        return -1;  /* no free pipe slots */

    /* Allocate two fds */
    int rfd = -1, wfd = -1;
    for (int i = 0; i < MAX_OPEN_FILES && (rfd < 0 || wfd < 0); i++) {
        if (!(t->fd_table[i].flags & FD_FLAG_USED)) {
            if (rfd < 0)
                rfd = i;
            else
                wfd = i;
        }
    }

    if (rfd < 0 || wfd < 0) {
        /* Not enough free fds — release the pipe */
        pipe_table[pidx].in_use = 0;
        return -1;
    }

    /* Set up read end */
    fd_entry_t *rf = &t->fd_table[rfd];
    rf->type     = FD_TYPE_PIPE;
    rf->flags    = FD_FLAG_USED | FD_FLAG_READABLE;
    rf->pipe_idx = (uint32_t)pidx;
    rf->ino      = 0;
    rf->offset   = 0;
    rf->file_size = 0;

    /* Set up write end */
    fd_entry_t *wf = &t->fd_table[wfd];
    wf->type     = FD_TYPE_PIPE;
    wf->flags    = FD_FLAG_USED | FD_FLAG_WRITABLE;
    wf->pipe_idx = (uint32_t)pidx;
    wf->ino      = 0;
    wf->offset   = 0;
    wf->file_size = 0;

    /* Set reference counts */
    pipe_table[pidx].readers = 1;
    pipe_table[pidx].writers = 1;

    pipefd[0] = rfd;
    pipefd[1] = wfd;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Signal syscalls                                                     */
/* ------------------------------------------------------------------ */

/* SYS_KILL (27): send a signal to a process.
 *   ebx = pid (0 = current process)
 *   ecx = signal number
 *   Returns: 0 on success, -1 on error. */
static int32_t sys_kill(struct isr_regs *regs) {
    int32_t  target_pid = (int32_t)regs->ebx;
    uint32_t sig        = regs->ecx;

    if (sig >= _NSIG)
        return -1;

    task_t *target;
    if (target_pid == 0) {
        target = get_current_task();
    } else if (target_pid > 0) {
        target = find_task_by_pid((uint32_t)target_pid);
    } else {
        /* pid < 0: send to process group (not implemented, send to all) */
        return -1;
    }

    if (!target)
        return -1;

    return task_send_signal(target, (int)sig);
}

/* SYS_SIGNAL (28): set a signal handler (simplified signal() interface).
 *   ebx = signal number
 *   ecx = new handler (SIG_DFL, SIG_IGN, or function pointer)
 *   Returns: previous handler, or SIG_ERR (-1) on error. */
static int32_t sys_signal(struct isr_regs *regs) {
    uint32_t sig = regs->ebx;
    sighandler_t handler = (sighandler_t)regs->ecx;

    if (sig < 1 || sig >= _NSIG)
        return (int32_t)(uint32_t)SIG_ERR;

    /* SIGKILL and SIGSTOP cannot be caught or ignored */
    if (sig == SIGKILL || sig == SIGSTOP)
        return (int32_t)(uint32_t)SIG_ERR;

    task_t *t = get_current_task();
    if (!t)
        return (int32_t)(uint32_t)SIG_ERR;

    sighandler_t old = t->sig_handlers[sig];
    t->sig_handlers[sig] = handler;

    return (int32_t)(uint32_t)old;
}

/* SYS_SIGRETURN (29): restore state after signal handler completes.
 *   Called by the signal trampoline code on the user stack.
 *
 *   Stack layout at sigreturn (after handler's ret + trampoline int 0x80):
 *     The trampoline does NOT pop the signal number; it simply does
 *     mov eax,173; int 0x80.  So the stack still has:
 *       useresp+0 → [signal number]
 *       useresp+4 → [ctx_addr pointer]
 *       ctx_addr  → [sigcontext_t with saved regs]
 */
static int32_t sys_sigreturn(struct isr_regs *regs) {
    task_t *t = get_current_task();
    if (!t)
        return -1;

    t->in_signal = 0;

    uint32_t *pd = t->page_dir;
    if (!pd)
        return -1;

    uint32_t user_sp = regs->useresp;
    /* Read ctx_addr from useresp + 4 (sig number is at useresp + 0) */
    uint32_t ctx_ptr_addr = user_sp + 4;
    uint32_t pv = ctx_ptr_addr & ~(SIG_PAGE_SIZE - 1);
    uint32_t ph = paging_get_physical_in(pd, pv);
    if (ph == 0) return -1;
    uint32_t ctx_addr = *(uint32_t *)(ph + (ctx_ptr_addr & (SIG_PAGE_SIZE - 1)));

    /* Read the sigcontext from user memory */
    sigcontext_t ctx;
    uint8_t *dst = (uint8_t *)&ctx;
    for (uint32_t i = 0; i < sizeof(sigcontext_t); i++) {
        uint32_t addr = ctx_addr + i;
        uint32_t pv2 = addr & ~(SIG_PAGE_SIZE - 1);
        uint32_t ph2 = paging_get_physical_in(pd, pv2);
        if (ph2 == 0) return -1;
        dst[i] = *(uint8_t *)(ph2 + (addr & (SIG_PAGE_SIZE - 1)));
    }

    /* Restore registers */
    regs->eax     = ctx.eax;
    regs->ebx     = ctx.ebx;
    regs->ecx     = ctx.ecx;
    regs->edx     = ctx.edx;
    regs->esi     = ctx.esi;
    regs->edi     = ctx.edi;
    regs->ebp     = ctx.ebp;
    regs->eip     = ctx.eip;
    regs->eflags  = (ctx.eflags | 0x200) & ~0x4000; /* keep IF set, clear NT */
    regs->useresp = ctx.esp;

    return (int32_t)ctx.eax;  /* restore original syscall return value */
}

/* SYS_SIGPROCMASK (30): get/set the signal block mask.
 *   ebx = how (0=SIG_BLOCK, 1=SIG_UNBLOCK, 2=SIG_SETMASK)
 *   ecx = pointer to new sigset_t (NULL = don't change)
 *   edx = pointer to old sigset_t (NULL = don't return)
 *   Returns: 0 on success, -1 on error. */
static int32_t sys_sigprocmask(struct isr_regs *regs) {
    uint32_t how       = regs->ebx;
    uint32_t *new_set  = (uint32_t *)regs->ecx;
    uint32_t *old_set  = (uint32_t *)regs->edx;

    task_t *t = get_current_task();
    if (!t)
        return -1;

    if (old_set)
        *old_set = t->sig_blocked;

    if (new_set) {
        uint32_t set = *new_set;
        /* Cannot block SIGKILL or SIGSTOP */
        set &= ~((1u << SIGKILL) | (1u << SIGSTOP));

        switch (how) {
        case 0:  /* SIG_BLOCK */
            t->sig_blocked |= set;
            break;
        case 1:  /* SIG_UNBLOCK */
            t->sig_blocked &= ~set;
            break;
        case 2:  /* SIG_SETMASK */
            t->sig_blocked = set;
            break;
        default:
            return -1;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  SYS_GETDENTS (31): read directory entries.                         */
/*    ebx = fd (must be an open directory)                             */
/*    ecx = pointer to user buffer                                     */
/*    edx = buffer size in bytes                                       */
/*  Returns: bytes written to buffer, 0 at EOF, -1 on error.          */
/*  Format per entry: struct moss_dirent {                             */
/*    uint32_t d_ino; uint16_t d_reclen; uint8_t d_type; char d_name[] */
/*  }                                                                  */
/* ------------------------------------------------------------------ */

static int32_t sys_getdents(struct isr_regs *regs) {
    uint32_t fd    = regs->ebx;
    uint8_t *ubuf  = (uint8_t *)regs->ecx;
    uint32_t bufsz = regs->edx;

    if (!ubuf || bufsz < 16)
        return -1;

    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES)
        return -1;

    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED) || !(f->flags & FD_FLAG_READABLE))
        return -1;
    if (f->type != FD_TYPE_FILE)
        return -1;

    /* Verify inode is a directory */
    ext2_inode_t dir;
    if (ext2_read_inode(f->ino, &dir) != 0)
        return -1;
    if (!(dir.i_mode & EXT2_S_IFDIR))
        return -1;

    /* Read directory entries starting from f->offset using ext2_read_file.
     * We read one block (1024 bytes) at a time and parse ext2 dir entries. */
    uint32_t dir_offset = f->offset;
    uint32_t buf_used = 0;

    while (dir_offset < dir.i_size) {
        /* Read a chunk of the directory */
        uint8_t blk_buf[1024];
        uint32_t to_read = 1024;
        if (dir_offset + to_read > dir.i_size)
            to_read = dir.i_size - dir_offset;

        int rn = ext2_read_file(f->ino, blk_buf, dir_offset, to_read);
        if (rn <= 0)
            break;

        uint32_t blk_off = 0;
        while (blk_off < (uint32_t)rn) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(blk_buf + blk_off);
            if (de->rec_len == 0)
                goto done;

            if (de->inode != 0) {
                /* Compute size of our moss_dirent structure:
                 * 4 (d_ino) + 2 (d_reclen) + 1 (d_type) + name_len + 1 (NUL) */
                uint32_t entry_size = 4 + 2 + 1 + de->name_len + 1;
                /* Align to 4 bytes */
                entry_size = (entry_size + 3) & ~3u;

                if (buf_used + entry_size > bufsz)
                    goto done;  /* buffer full */

                /* Write d_ino */
                *(uint32_t *)(ubuf + buf_used) = de->inode;
                /* Write d_reclen */
                *(uint16_t *)(ubuf + buf_used + 4) = (uint16_t)entry_size;
                /* Write d_type */
                ubuf[buf_used + 6] = de->file_type;
                /* Write d_name (NUL-terminated) */
                memcpy(ubuf + buf_used + 7, de->name, de->name_len);
                ubuf[buf_used + 7 + de->name_len] = '\0';

                buf_used += entry_size;
            }

            dir_offset += de->rec_len;
            blk_off += de->rec_len;
        }
    }

done:
    f->offset = dir_offset;
    return (int32_t)buf_used;
}

/* SYS_ISATTY (32): check if fd refers to a terminal.
 *   ebx = fd
 *   Returns: 1 if terminal, 0 otherwise. */
static int32_t sys_isatty(struct isr_regs *regs) {
    uint32_t fd = regs->ebx;

    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES)
        return 0;

    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED))
        return 0;

    /* stdin, stdout, stderr, /dev/tty are terminals */
    if (f->type == FD_TYPE_STDIN || f->type == FD_TYPE_STDOUT ||
        f->type == FD_TYPE_STDERR || f->type == FD_TYPE_DEVTTY)
        return 1;

    return 0;
}

/* SYS_FCNTL (33): file descriptor control.
 *   ebx = fd
 *   ecx = cmd (F_GETFD, F_SETFD, F_GETFL, F_SETFL, F_DUPFD)
 *   edx = arg (command-specific)
 *   Returns: depends on cmd, -1 on error. */
#define F_DUPFD  0
#define F_GETFD  1
#define F_SETFD  2
#define F_GETFL  3
#define F_SETFL  4
#define FD_CLOEXEC 1

static int32_t sys_fcntl(struct isr_regs *regs) {
    uint32_t fd  = regs->ebx;
    uint32_t cmd = regs->ecx;

    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES)
        return -1;

    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED))
        return -1;

    switch (cmd) {
    case F_GETFD:
        return 0;  /* no close-on-exec tracking yet */
    case F_SETFD:
        return 0;  /* silently accept */
    case F_GETFL:
        /* Return O_RDONLY/O_WRONLY/O_RDWR based on flags */
        if ((f->flags & FD_FLAG_READABLE) && (f->flags & FD_FLAG_WRITABLE))
            return 2;  /* O_RDWR */
        if (f->flags & FD_FLAG_WRITABLE)
            return 1;  /* O_WRONLY */
        return 0;  /* O_RDONLY */
    case F_SETFL:
        return 0;  /* silently accept */
    case F_DUPFD:
        /* Find lowest available fd >= arg */
        {
            uint32_t start = regs->edx;
            if (start >= MAX_OPEN_FILES)
                return -1;
            for (uint32_t i = start; i < MAX_OPEN_FILES; i++) {
                if (!(t->fd_table[i].flags & FD_FLAG_USED)) {
                    t->fd_table[i] = *f;
                    /* Handle pipe refcount */
                    if (f->type == FD_TYPE_PIPE) {
                        pipe_t *p = &pipe_table[f->pipe_idx];
                        if (f->flags & FD_FLAG_READABLE) p->readers++;
                        if (f->flags & FD_FLAG_WRITABLE) p->writers++;
                    }
                    return (int32_t)i;
                }
            }
        }
        return -1;
    default:
        return -1;
    }
}

/* SYS_GETPPID (34): get parent PID.
 *   Returns: parent PID. */
static int32_t sys_getppid(struct isr_regs *regs) {
    (void)regs;
    task_t *t = get_current_task();
    if (!t)
        return 0;
    return (int32_t)t->parent_pid;
}

/* SYS_SETENV (35): set an environment variable.
 *   ebx = pointer to "KEY=VALUE" string
 *   Returns: 0 on success, -1 on error. */
static int32_t sys_setenv(struct isr_regs *regs) {
    const char *kv = (const char *)regs->ebx;
    if (!kv) return -1;

    env_init();

    /* Find the '=' separator */
    const char *eq = kv;
    while (*eq && *eq != '=') eq++;
    if (*eq != '=') return -1;

    size_t keylen = (size_t)(eq - kv);
    size_t total_len = strlen(kv) + 1;

    /* Check if key already exists */
    int idx = env_find(kv, keylen);
    if (idx >= 0) {
        /* Replace: if new value fits in old slot's space, reuse it.
         * Otherwise, just append and waste old space (simple approach). */
        size_t old_len = strlen(env_vars[idx]) + 1;
        if (total_len <= old_len) {
            memcpy(env_vars[idx], kv, total_len);
            return 0;
        }
        /* Can't reuse — append at end */
        if (env_buf_used + (int)total_len > ENV_BUF_SIZE)
            return -1;
        env_vars[idx] = env_buf + env_buf_used;
        memcpy(env_buf + env_buf_used, kv, total_len);
        env_buf_used += (int)total_len;
        return 0;
    }

    /* New variable */
    if (env_count >= ENV_MAX_VARS)
        return -1;
    if (env_buf_used + (int)total_len > ENV_BUF_SIZE)
        return -1;

    env_vars[env_count] = env_buf + env_buf_used;
    memcpy(env_buf + env_buf_used, kv, total_len);
    env_buf_used += (int)total_len;
    env_count++;
    return 0;
}

/* SYS_GETENV (36): get an environment variable value.
 *   ebx = pointer to key string
 *   ecx = pointer to output buffer
 *   edx = buffer size
 *   Returns: length of value (excluding NUL), or -1 if not found. */
static int32_t sys_getenv(struct isr_regs *regs) {
    const char *key = (const char *)regs->ebx;
    char *out = (char *)regs->ecx;
    uint32_t outsz = regs->edx;

    if (!key) return -1;

    env_init();

    size_t keylen = strlen(key);
    int idx = env_find(key, keylen);
    if (idx < 0)
        return -1;

    const char *value = env_vars[idx] + keylen + 1;  /* skip "KEY=" */
    size_t vlen = strlen(value);

    if (out && outsz > 0) {
        size_t copy = vlen;
        if (copy >= outsz) copy = outsz - 1;
        memcpy(out, value, copy);
        out[copy] = '\0';
    }

    return (int32_t)vlen;
}

/* SYS_MKDIR (37): create a directory.
 *   ebx = pointer to path string
 *   Returns: 0 on success, -1 on error. */
static int32_t sys_mkdir(struct isr_regs *regs) {
    const char *path = (const char *)regs->ebx;
    if (!path || path[0] == '\0')
        return -1;

    /* Split path into parent directory + new directory name */
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/')
            last_slash = p;

    uint32_t parent_ino;
    const char *dirname;
    if (last_slash) {
        char parent_path[256];
        int plen = (int)(last_slash - path);
        if (plen == 0) {
            parent_ino = EXT2_ROOT_INO;
        } else {
            if (plen > 255) plen = 255;
            memcpy(parent_path, path, plen);
            parent_path[plen] = '\0';
            parent_ino = resolve_path(parent_path);
        }
        dirname = last_slash + 1;
    } else {
        task_t *t = get_current_task();
        parent_ino = t ? t->cwd_ino : EXT2_ROOT_INO;
        dirname = path;
    }

    if (parent_ino == 0 || dirname[0] == '\0')
        return -1;

    /* Check write permission on parent directory */
    {
        task_t *t2 = get_current_task();
        if (t2) {
            ext2_inode_t dir_inode;
            if (ext2_read_inode(parent_ino, &dir_inode) == 0) {
                if (!check_permission(t2->euid, t2->egid, dir_inode.i_uid, dir_inode.i_gid,
                                      dir_inode.i_mode & 0xFFF, W_OK))
                    return -1;
            }
        }
    }

    uint32_t new_ino = ext2_create(parent_ino, dirname, EXT2_S_IFDIR | 0755);
    if (new_ino == 0)
        return -1;

    return 0;
}

/* ------------------------------------------------------------------ */
/*  User identity syscalls                                             */
/* ------------------------------------------------------------------ */

/* SYS_GETUID (42): get real user ID.
 *   Returns: uid of the calling process. */
static int32_t sys_getuid(struct isr_regs *regs) {
    (void)regs;
    task_t *t = get_current_task();
    return t ? (int32_t)t->uid : -1;
}

/* SYS_SETUID (43): set real and effective user ID.
 *   ebx = uid
 *   Returns: 0 on success, -1 on error (only root can change). */
static int32_t sys_setuid(struct isr_regs *regs) {
    uint16_t new_uid = (uint16_t)regs->ebx;
    task_t *t = get_current_task();
    if (!t)
        return -1;
    /* Only root (euid == 0) can setuid to arbitrary uid */
    if (t->euid != UID_ROOT && new_uid != t->uid)
        return -1;
    t->uid  = new_uid;
    t->euid = new_uid;
    return 0;
}

/* SYS_GETGID (44): get real group ID.
 *   Returns: gid of the calling process. */
static int32_t sys_getgid(struct isr_regs *regs) {
    (void)regs;
    task_t *t = get_current_task();
    return t ? (int32_t)t->gid : -1;
}

/* SYS_SETGID (45): set real and effective group ID.
 *   ebx = gid
 *   Returns: 0 on success, -1 on error (only root can change). */
static int32_t sys_setgid(struct isr_regs *regs) {
    uint16_t new_gid = (uint16_t)regs->ebx;
    task_t *t = get_current_task();
    if (!t)
        return -1;
    if (t->euid != UID_ROOT && new_gid != t->gid)
        return -1;
    t->gid  = new_gid;
    t->egid = new_gid;
    return 0;
}

/* SYS_CHMOD (46): change file permissions.
 *   ebx = pointer to path
 *   ecx = new mode (lower 12 bits)
 *   Returns: 0 on success, -1 on error. */
static int32_t sys_chmod(struct isr_regs *regs) {
    const char *path = (const char *)regs->ebx;
    uint16_t new_mode = (uint16_t)(regs->ecx & 0x0FFF);
    if (!path)
        return -1;

    task_t *t = get_current_task();
    if (!t)
        return -1;

    uint32_t ino = resolve_path(path);
    if (ino == 0)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;

    /* Only owner or root can chmod */
    if (t->euid != UID_ROOT && t->euid != inode.i_uid)
        return -1;

    inode.i_mode = (inode.i_mode & 0xF000) | new_mode;
    if (ext2_write_inode(ino, &inode) != 0)
        return -1;

    return 0;
}

/* SYS_CHOWN (47): change file owner and group.
 *   ebx = pointer to path
 *   ecx = new uid (0xFFFF = don't change)
 *   edx = new gid (0xFFFF = don't change)
 *   Returns: 0 on success, -1 on error. */
static int32_t sys_chown(struct isr_regs *regs) {
    const char *path = (const char *)regs->ebx;
    uint16_t new_uid = (uint16_t)regs->ecx;
    uint16_t new_gid = (uint16_t)regs->edx;
    if (!path)
        return -1;

    task_t *t = get_current_task();
    if (!t)
        return -1;

    /* Only root can chown */
    if (t->euid != UID_ROOT)
        return -1;

    uint32_t ino = resolve_path(path);
    if (ino == 0)
        return -1;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;

    if (new_uid != 0xFFFF)
        inode.i_uid = new_uid;
    if (new_gid != 0xFFFF)
        inode.i_gid = new_gid;

    if (ext2_write_inode(ino, &inode) != 0)
        return -1;

    return 0;
}

/* SYS_GETEUID (48): get effective user ID. */
static int32_t sys_geteuid(struct isr_regs *regs) {
    (void)regs;
    task_t *t = get_current_task();
    return t ? (int32_t)t->euid : -1;
}

/* SYS_GETEGID (49): get effective group ID. */
static int32_t sys_getegid(struct isr_regs *regs) {
    (void)regs;
    task_t *t = get_current_task();
    return t ? (int32_t)t->egid : -1;
}

typedef int32_t (*syscall_fn_t)(struct isr_regs *regs);

/* ------------------------------------------------------------------ */
/*  New syscalls for Linux/musl compatibility                          */
/* ------------------------------------------------------------------ */

/* SYS_MMAP2 (192): memory-mapped allocation.
 *   ebx = addr (hint, usually 0)
 *   ecx = length
 *   edx = prot (PROT_READ|PROT_WRITE|PROT_EXEC)
 *   esi = flags (MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED)
 *   edi = fd (-1 for anonymous)
 *   ebp = offset (in pages, i.e. offset_bytes / 4096)
 *   Returns: mapped address, or -errno on error. */

/* Track mmap region base for sequential allocations */
static uint32_t mmap_next_addr = 0x40000000u;

static int32_t sys_mmap2(struct isr_regs *regs) {
    uint32_t addr   = regs->ebx;
    uint32_t length = regs->ecx;
    uint32_t prot   = regs->edx;
    uint32_t flags  = regs->esi;
    int32_t  fd     = (int32_t)regs->edi;
    uint32_t pgoff  = regs->ebp;

    (void)prot; /* We always map read/write/user for now */
    (void)pgoff;

    task_t *t = get_current_task();
    if (!t || !t->page_dir)
        return -12; /* -ENOMEM */

    if (length == 0)
        return -22; /* -EINVAL */

    /* Round up to page size */
    uint32_t num_pages = (length + SYS_PAGE_SIZE - 1) / SYS_PAGE_SIZE;

    /* Determine where to map */
    uint32_t map_addr;
    if ((flags & 0x10) && addr) {  /* MAP_FIXED */
        map_addr = addr & ~(SYS_PAGE_SIZE - 1);
    } else if (addr) {
        map_addr = addr & ~(SYS_PAGE_SIZE - 1);
    } else {
        map_addr = mmap_next_addr;
    }

    /* Sanity: don't overlap kernel space or go below userland */
    if (map_addr < 0x10000u || map_addr >= 0xC0000000u)
        return -12; /* -ENOMEM */
    if (map_addr + num_pages * SYS_PAGE_SIZE < map_addr)
        return -12; /* overflow */
    if (map_addr + num_pages * SYS_PAGE_SIZE > 0xBF000000u)
        return -12;

    /* If anonymous mapping, allocate physical pages and map them */
    if (flags & 0x20) { /* MAP_ANONYMOUS */
        for (uint32_t i = 0; i < num_pages; i++) {
            uint32_t vaddr = map_addr + i * SYS_PAGE_SIZE;
            void *page = elf_map_user_page_in(vaddr, t->page_dir,
                                              t->user_pages_slot);
            if (!page) {
                /* Partial allocation; pages already mapped are leaked—
                 * simplified; a proper impl would unwind. */
                return -12; /* -ENOMEM */
            }
        }
        /* Advance next_addr past this allocation */
        uint32_t end = map_addr + num_pages * SYS_PAGE_SIZE;
        if (end > mmap_next_addr)
            mmap_next_addr = end;
        return (int32_t)map_addr;
    }

    /* File-backed mmap: read from fd into freshly mapped pages */
    if (fd >= 0) {
        if ((uint32_t)fd >= MAX_OPEN_FILES)
            return -9; /* -EBADF */
        fd_entry_t *f = &t->fd_table[fd];
        if (!(f->flags & FD_FLAG_USED) || f->type != FD_TYPE_FILE)
            return -9;

        for (uint32_t i = 0; i < num_pages; i++) {
            uint32_t vaddr = map_addr + i * SYS_PAGE_SIZE;
            void *page = elf_map_user_page_in(vaddr, t->page_dir,
                                              t->user_pages_slot);
            if (!page)
                return -12;

            /* Read file content into this page */
            uint32_t file_offset = (pgoff + i) * SYS_PAGE_SIZE;
            if (file_offset < f->file_size) {
                uint32_t to_read = SYS_PAGE_SIZE;
                if (file_offset + to_read > f->file_size)
                    to_read = f->file_size - file_offset;
                ext2_read_file(f->ino, (char *)page, file_offset, to_read);
            }
        }
        uint32_t end = map_addr + num_pages * SYS_PAGE_SIZE;
        if (end > mmap_next_addr)
            mmap_next_addr = end;
        return (int32_t)map_addr;
    }

    return -22; /* -EINVAL */
}

/* SYS_MMAP (90): old-style mmap — args passed via pointer in ebx.
 *   ebx = pointer to uint32_t[6] = {addr, length, prot, flags, fd, offset}
 *   Note: offset is in BYTES (not pages like mmap2). */
static int32_t sys_old_mmap(struct isr_regs *regs) {
    uint32_t *args = (uint32_t *)regs->ebx;
    if (!args) return -14;

    /* Repack into the register layout that sys_mmap2 expects */
    struct isr_regs fake = *regs;
    fake.ebx = args[0]; /* addr */
    fake.ecx = args[1]; /* length */
    fake.edx = args[2]; /* prot */
    fake.esi = args[3]; /* flags */
    fake.edi = args[4]; /* fd */
    fake.ebp = args[5] / SYS_PAGE_SIZE; /* offset in pages */
    return sys_mmap2(&fake);
}

/* SYS_MUNMAP (91): unmap pages.
 *   ebx = addr
 *   ecx = length
 *   Returns: 0 on success, -errno on error. */
static int32_t sys_munmap(struct isr_regs *regs) {
    uint32_t addr   = regs->ebx;
    uint32_t length = regs->ecx;

    if (length == 0 || (addr & (SYS_PAGE_SIZE - 1)))
        return -22; /* -EINVAL */

    /* For now, accept the call but don't actually free pages.
     * A full implementation would remove pages from the slot tracking
     * and free the physical memory.  This is safe because leaked pages
     * are reclaimed on process exit via elf_cleanup_process(). */
    (void)addr;
    (void)length;
    return 0;
}

/* SYS_MPROTECT (125): change protection on a region.
 *   ebx = addr, ecx = len, edx = prot
 *   Returns: 0 (stub — we don't enforce page-level protections). */
static int32_t sys_mprotect(struct isr_regs *regs) {
    (void)regs;
    return 0;
}

/* SYS_IOCTL (54): device control.
 *   ebx = fd, ecx = request, edx = argp
 *   Returns: 0 on success, -errno on error. */
static int32_t sys_ioctl(struct isr_regs *regs) {
    uint32_t fd  = regs->ebx;
    uint32_t req = regs->ecx;
    void    *arg = (void *)regs->edx;

    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES)
        return -9; /* -EBADF */

    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED))
        return -9;

    /* Terminal ioctls */
    int is_tty = (f->type == FD_TYPE_STDIN || f->type == FD_TYPE_STDOUT ||
                  f->type == FD_TYPE_STDERR || f->type == FD_TYPE_DEVTTY);

    switch (req) {
    case TCGETS: { /* 0x5401 */
        if (!is_tty) return -25; /* -ENOTTY */
        struct kernel_termios {
            uint32_t c_iflag;
            uint32_t c_oflag;
            uint32_t c_cflag;
            uint32_t c_lflag;
            uint8_t  c_line;
            uint8_t  c_cc[19];
        } *kt = (struct kernel_termios *)arg;
        if (!kt) return -14; /* -EFAULT */
        memset(kt, 0, sizeof(*kt));
        /* Report canonical mode with echo */
        kt->c_lflag = 0x000B; /* ICANON|ECHO|ISIG */
        kt->c_iflag = 0x0002; /* ICRNL */
        kt->c_oflag = 0x0001; /* OPOST */
        kt->c_cflag = 0x00BF; /* CS8 | B38400 | CREAD */
        kt->c_cc[6]  = 1;    /* VMIN */
        kt->c_cc[7]  = 0;    /* VTIME */
        kt->c_cc[4]  = 4;    /* VEOF = Ctrl-D */
        return 0;
    }
    case TCSETS:   /* 0x5402 */
    case TCSETSW:  /* 0x5403 */
    case TCSETSF:  /* 0x5404 */
        if (!is_tty) return -25;
        /* Accept but ignore — we don't change TTY modes yet */
        return 0;

    case TIOCGWINSZ: { /* 0x5413 */
        if (!is_tty) return -25;
        struct winsize {
            uint16_t ws_row;
            uint16_t ws_col;
            uint16_t ws_xpixel;
            uint16_t ws_ypixel;
        } *ws = (struct winsize *)arg;
        if (!ws) return -14;
        ws->ws_row = (uint16_t)terminal_get_rows();
        ws->ws_col = (uint16_t)terminal_get_cols();
        ws->ws_xpixel = 0;
        ws->ws_ypixel = 0;
        return 0;
    }
    case TIOCSWINSZ: /* 0x5414 */
        return 0; /* ignore */

    case TIOCGPGRP: { /* 0x540F */
        if (!is_tty) return -25;
        int *pgrp = (int *)arg;
        if (pgrp) *pgrp = (int)t->pid; /* stub: return own pid */
        return 0;
    }
    case TIOCSPGRP: /* 0x5410 */
        return 0; /* ignore */

    default:
        return -25; /* -ENOTTY (unsupported ioctl) */
    }
}

/* SYS_WRITEV (146): write scattered buffers.
 *   ebx = fd, ecx = iov pointer, edx = iovcnt
 *   Returns: total bytes written, or -errno. */
static int32_t sys_writev(struct isr_regs *regs) {
    uint32_t fd     = regs->ebx;
    struct { uint32_t base; uint32_t len; } *iov =
        (void *)regs->ecx;
    int32_t iovcnt = (int32_t)regs->edx;

    if (!iov || iovcnt <= 0)
        return -22;

    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES)
        return -9;

    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED) || !(f->flags & FD_FLAG_WRITABLE))
        return -9;

    int32_t total = 0;
    for (int32_t i = 0; i < iovcnt; i++) {
        const char *buf = (const char *)iov[i].base;
        uint32_t count = iov[i].len;
        if (!buf || count == 0)
            continue;

        if (f->type == FD_TYPE_STDOUT || f->type == FD_TYPE_STDERR ||
            f->type == FD_TYPE_DEVTTY) {
            terminal_write(buf, (size_t)count);
            total += (int32_t)count;
        } else if (f->type == FD_TYPE_DEVNULL) {
            total += (int32_t)count;
        } else if (f->type == FD_TYPE_FILE) {
            int written = ext2_write_file(f->ino, buf, f->offset, count);
            if (written > 0) {
                f->offset += (uint32_t)written;
                if (f->offset > f->file_size)
                    f->file_size = f->offset;
                total += written;
            }
        } else if (f->type == FD_TYPE_PIPE) {
            pipe_t *p = &pipe_table[f->pipe_idx];
            if (p->readers == 0) {
                task_send_signal(t, SIGPIPE);
                return total > 0 ? total : -32; /* -EPIPE */
            }
            uint32_t written = 0;
            while (written < count) {
                if (p->count < PIPE_BUF_SIZE) {
                    uint32_t space = PIPE_BUF_SIZE - p->count;
                    uint32_t chunk = count - written;
                    if (chunk > space) chunk = space;
                    for (uint32_t j = 0; j < chunk; j++) {
                        p->buf[p->write_pos] = (uint8_t)buf[written + j];
                        p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
                    }
                    p->count += chunk;
                    written += chunk;
                } else {
                    t->state = TASK_INTERRUPTIBLE;
                    schedule();
                }
            }
            total += (int32_t)written;
        }
    }
    return total;
}

/* SYS_READV (145): read into scattered buffers (stub using sys_read logic). */
static int32_t sys_readv(struct isr_regs *regs) {
    /* For now just handle it simply */
    (void)regs;
    return -38; /* -ENOSYS */
}

/* SYS_ACCESS (33): check file access permissions.
 *   ebx = path, ecx = mode (F_OK, R_OK, W_OK, X_OK)
 *   Returns: 0 on success, -errno on error. */
static int32_t sys_access(struct isr_regs *regs) {
    const char *path = (const char *)regs->ebx;
    uint32_t mode    = regs->ecx;

    if (!path)
        return -14; /* -EFAULT */

    uint32_t ino = resolve_path(path);
    if (ino == 0)
        return -2; /* -ENOENT */

    if (mode == 0) /* F_OK — just check existence */
        return 0;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -2;

    task_t *t = get_current_task();
    uint16_t uid = t ? t->uid : 0;
    uint16_t gid = t ? t->gid : 0;

    if (!check_permission(uid, gid, inode.i_uid, inode.i_gid,
                          inode.i_mode & 0xFFF, (int)mode))
        return -13; /* -EACCES */

    return 0;
}

/* SYS_UMASK (60): set file creation mask.
 *   ebx = new umask
 *   Returns: old umask. */
static uint32_t process_umask = 0022;

static int32_t sys_umask(struct isr_regs *regs) {
    uint32_t new_mask = regs->ebx & 0777;
    uint32_t old_mask = process_umask;
    process_umask = new_mask;
    return (int32_t)old_mask;
}

/* SYS_RENAME (38): rename a file.
 *   ebx = old path, ecx = new path
 *   Returns: 0 on success, -errno on error. */
static int32_t sys_rename(struct isr_regs *regs) {
    const char *oldpath = (const char *)regs->ebx;
    const char *newpath = (const char *)regs->ecx;
    if (!oldpath || !newpath)
        return -14;

    /* Resolve old file */
    uint32_t old_ino = resolve_path(oldpath);
    if (old_ino == 0)
        return -2; /* -ENOENT */

    /* Find parent dir and name for new path */
    const char *last_slash = NULL;
    for (const char *p = newpath; *p; p++)
        if (*p == '/') last_slash = p;

    uint32_t new_parent_ino;
    const char *new_name;
    if (last_slash) {
        char parent_path[256];
        int plen = (int)(last_slash - newpath);
        if (plen == 0) {
            new_parent_ino = EXT2_ROOT_INO;
        } else {
            if (plen > 255) plen = 255;
            memcpy(parent_path, newpath, plen);
            parent_path[plen] = '\0';
            new_parent_ino = resolve_path(parent_path);
        }
        new_name = last_slash + 1;
    } else {
        task_t *t = get_current_task();
        new_parent_ino = t ? t->cwd_ino : EXT2_ROOT_INO;
        new_name = newpath;
    }

    if (new_parent_ino == 0 || new_name[0] == '\0')
        return -2;

    /* If target already exists, remove it first */
    uint32_t exist_ino = ext2_lookup(new_parent_ino, new_name);
    if (exist_ino != 0)
        ext2_remove(new_parent_ino, new_name);

    /* Read old inode to get type */
    ext2_inode_t old_inode;
    if (ext2_read_inode(old_ino, &old_inode) != 0)
        return -5; /* -EIO */

    uint8_t file_type = 1; /* regular */
    if ((old_inode.i_mode & 0xF000) == EXT2_S_IFDIR)
        file_type = 2; /* directory */

    /* Add a directory entry in new parent pointing to old inode */
    if (ext2_link(new_parent_ino, old_ino, new_name, file_type) != 0)
        return -28; /* -ENOSPC */

    /* Remove old directory entry */
    const char *old_last_slash = NULL;
    for (const char *p = oldpath; *p; p++)
        if (*p == '/') old_last_slash = p;

    uint32_t old_parent_ino;
    const char *old_name;
    if (old_last_slash) {
        char parent_path[256];
        int plen = (int)(old_last_slash - oldpath);
        if (plen == 0) {
            old_parent_ino = EXT2_ROOT_INO;
        } else {
            if (plen > 255) plen = 255;
            memcpy(parent_path, oldpath, plen);
            parent_path[plen] = '\0';
            old_parent_ino = resolve_path(parent_path);
        }
        old_name = old_last_slash + 1;
    } else {
        task_t *t = get_current_task();
        old_parent_ino = t ? t->cwd_ino : EXT2_ROOT_INO;
        old_name = oldpath;
    }

    ext2_unlink(old_parent_ino, old_name);
    return 0;
}

/* SYS_NANOSLEEP (162): sleep with nanosecond precision.
 *   ebx = pointer to struct timespec {time_t sec; long nsec;}
 *   ecx = pointer to remaining time (can be NULL)
 *   Returns: 0 on success. */
static int32_t sys_nanosleep(struct isr_regs *regs) {
    struct { uint32_t tv_sec; uint32_t tv_nsec; } *req =
        (void *)regs->ebx;

    if (!req)
        return -14;

    uint32_t ms = req->tv_sec * 1000 + req->tv_nsec / 1000000;
    if (ms == 0 && req->tv_nsec > 0)
        ms = 1; /* minimum 1ms */

    uint32_t start = timer_get_ticks();
    while (timer_get_ticks() - start < ms) {
        task_t *me = get_current_task();
        if (me) {
            me->state = TASK_INTERRUPTIBLE;
            schedule();
        } else {
            asm volatile("sti; hlt");
        }
    }

    /* Fill remaining time as 0 */
    struct { uint32_t tv_sec; uint32_t tv_nsec; } *rem =
        (void *)regs->ecx;
    if (rem) {
        rem->tv_sec = 0;
        rem->tv_nsec = 0;
    }
    return 0;
}

/* SYS_CLOCK_GETTIME (265): get clock time.
 *   ebx = clock_id (CLOCK_REALTIME or CLOCK_MONOTONIC)
 *   ecx = pointer to struct timespec
 *   Returns: 0 on success, -errno. */
static int32_t sys_clock_gettime(struct isr_regs *regs) {
    uint32_t clock_id = regs->ebx;
    struct { uint32_t tv_sec; uint32_t tv_nsec; } *tp =
        (void *)regs->ecx;

    if (!tp)
        return -14;

    (void)clock_id; /* treat all clocks as monotonic for now */
    uint32_t ticks = timer_get_ticks(); /* 1000 Hz */
    tp->tv_sec = ticks / 1000;
    tp->tv_nsec = (ticks % 1000) * 1000000;
    return 0;
}

/* SYS_CLOCK_GETTIME64 (403): 64-bit time version. */
static int32_t sys_clock_gettime64(struct isr_regs *regs) {
    uint32_t clock_id = regs->ebx;
    struct { uint32_t tv_sec_lo; uint32_t tv_sec_hi; uint32_t tv_nsec_lo; uint32_t tv_nsec_hi; } *tp =
        (void *)regs->ecx;

    if (!tp)
        return -14;

    (void)clock_id;
    uint32_t ticks = timer_get_ticks();
    tp->tv_sec_lo = ticks / 1000;
    tp->tv_sec_hi = 0;
    tp->tv_nsec_lo = (ticks % 1000) * 1000000;
    tp->tv_nsec_hi = 0;
    return 0;
}

/* SYS_SET_THREAD_AREA (243): set up TLS for the calling thread.
 *   ebx = pointer to struct user_desc
 *   Returns: 0 on success, -errno. */
extern int gdt_set_tls(int entry_number, uint32_t base, uint32_t limit);
extern void gdt_load_gs(uint16_t selector);

static int32_t sys_set_thread_area(struct isr_regs *regs) {
    struct user_desc {
        uint32_t entry_number;
        uint32_t base_addr;
        uint32_t limit;
        uint32_t flags; /* seg_32bit:1, contents:2, read_exec_only:1,
                           limit_in_pages:1, seg_not_present:1, useable:1 */
    } *u = (struct user_desc *)regs->ebx;

    if (!u)
        return -14;

    int entry = (int)u->entry_number;

    /* musl passes -1 to request a free entry */
    if (entry == -1) {
        entry = 6; /* Use first TLS slot */
        u->entry_number = 6;
    }

    /* Linux TLS entries are 6, 7, 8 in the GDT */
    if (entry < 6 || entry > 8)
        return -22; /* -EINVAL */

    uint32_t limit = u->limit;
    /* If limit_in_pages flag is set, limit is in 4K pages */
    if (u->flags & 0x10)
        limit = (limit << 12) | 0xFFF;

    int ret = gdt_set_tls(entry, u->base_addr, limit);
    if (ret < 0)
        return -22;

    /* Update the saved GS on the ISR stack frame so it will be
     * restored by the ISR return path (ring 3 selector). */
    uint16_t selector = (uint16_t)((entry << 3) | 3);
    regs->gs = selector;

    /* Save TLS parameters in the task struct for context switches */
    task_t *t = get_current_task();
    if (t) {
        t->tls_gs    = selector;
        t->tls_entry = entry;
        t->tls_base  = u->base_addr;
        t->tls_limit = limit;
    }

    return 0;
}

/* SYS_EXIT_GROUP (252): exit all threads (same as exit for single-threaded). */
static int32_t sys_exit_group(struct isr_regs *regs) {
    return sys_exit(regs);
}

/* SYS_RT_SIGACTION (174): set signal action (extended).
 *   ebx = signum
 *   ecx = pointer to new sigaction (or NULL)
 *   edx = pointer to old sigaction (or NULL)
 *   esi = sigsetsize
 *   Returns: 0 on success, -errno. */
static int32_t sys_rt_sigaction(struct isr_regs *regs) {
    uint32_t sig = regs->ebx;
    struct kernel_sigaction {
        uint32_t handler;   /* sa_handler or sa_sigaction */
        uint32_t sa_flags;
        uint32_t sa_restorer;
        uint32_t sa_mask[2]; /* 64-bit signal mask */
    } *act = (void *)regs->ecx, *oact = (void *)regs->edx;

    if (sig < 1 || sig >= _NSIG)
        return -22;
    if (sig == SIGKILL || sig == SIGSTOP)
        return -22;

    task_t *t = get_current_task();
    if (!t)
        return -22;

    if (oact) {
        oact->handler = (uint32_t)t->sig_handlers[sig];
        oact->sa_flags = 0;
        oact->sa_restorer = 0;
        oact->sa_mask[0] = 0;
        oact->sa_mask[1] = 0;
    }

    if (act) {
        t->sig_handlers[sig] = (sighandler_t)act->handler;
    }

    return 0;
}

/* SYS_RT_SIGPROCMASK (175): same semantics as old sigprocmask but with
 * explicit sigsetsize parameter.
 *   ebx = how, ecx = new set ptr, edx = old set ptr, esi = sigsetsize */
static int32_t sys_rt_sigprocmask(struct isr_regs *regs) {
    /* Delegate to existing implementation; just ignore sigsetsize for now */
    return sys_sigprocmask(regs);
}

/* SYS_RT_SIGRETURN (173): extended sigreturn.
 * Same as the old sigreturn for us. */
static int32_t sys_rt_sigreturn(struct isr_regs *regs) {
    return sys_sigreturn(regs);
}

/* SYS_GETCWD (183): already implemented above as sys_getcwd */

/* SYS_STAT64/LSTAT64/FSTAT64 (195/196/197): stat with 64-bit fields.
 * Linux i386 stat64 struct layout (from kernel). */
struct linux_stat64 {
    uint64_t st_dev;        /* 0 */
    uint32_t __pad1;        /* 8 */
    uint32_t __st_ino;      /* 12 */
    uint32_t st_mode;       /* 16 */
    uint32_t st_nlink;      /* 20 */
    uint32_t st_uid;        /* 24 */
    uint32_t st_gid;        /* 28 */
    uint64_t st_rdev;       /* 32 */
    uint32_t __pad2;        /* 40 */
    int64_t  st_size;       /* 44 - NOTE: actually at offset 44 for i386 */
    uint32_t st_blksize;    /* 52 */
    uint64_t st_blocks;     /* 56 */
    uint32_t st_atime_;     /* 64 */
    uint32_t st_atime_nsec; /* 68 */
    uint32_t st_mtime_;     /* 72 */
    uint32_t st_mtime_nsec; /* 76 */
    uint32_t st_ctime_;     /* 80 */
    uint32_t st_ctime_nsec; /* 84 */
    uint64_t st_ino;        /* 88 */
} __attribute__((packed));

static void fill_stat64(uint32_t ino, const ext2_inode_t *in, struct linux_stat64 *st) {
    memset(st, 0, sizeof(*st));
    st->st_dev = 0;
    st->__st_ino = ino;
    st->st_ino = ino;
    st->st_mode = in->i_mode;
    st->st_nlink = in->i_links_count;
    st->st_uid = in->i_uid;
    st->st_gid = in->i_gid;
    st->st_rdev = 0;
    st->st_size = in->i_size;
    st->st_blksize = 1024;
    st->st_blocks = in->i_blocks;
    st->st_atime_ = in->i_atime;
    st->st_mtime_ = in->i_mtime;
    st->st_ctime_ = in->i_ctime;
}

static int32_t sys_stat64(struct isr_regs *regs) {
    const char *path = (const char *)regs->ebx;
    struct linux_stat64 *st = (struct linux_stat64 *)regs->ecx;
    if (!path || !st) return -14;

    uint32_t ino = resolve_path(path);
    if (ino == 0) return -2;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) return -5;

    fill_stat64(ino, &inode, st);
    return 0;
}

static int32_t sys_lstat64(struct isr_regs *regs) {
    /* No symlinks yet — same as stat64 */
    return sys_stat64(regs);
}

static int32_t sys_fstat64(struct isr_regs *regs) {
    uint32_t fd = regs->ebx;
    struct linux_stat64 *st = (struct linux_stat64 *)regs->ecx;
    if (!st) return -14;

    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES) return -9;

    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED)) return -9;

    /* For stdin/stdout/stderr, return a character device stat */
    if (f->type == FD_TYPE_STDIN || f->type == FD_TYPE_STDOUT ||
        f->type == FD_TYPE_STDERR) {
        memset(st, 0, sizeof(*st));
        st->st_mode = 0020666; /* S_IFCHR | 0666 */
        st->st_rdev = 0x0501;  /* /dev/tty */
        st->st_blksize = 1024;
        return 0;
    }

    /* For pipes */
    if (f->type == FD_TYPE_PIPE) {
        memset(st, 0, sizeof(*st));
        st->st_mode = 0010600; /* S_IFIFO | 0600 */
        st->st_blksize = 4096;
        return 0;
    }

    if (f->type != FD_TYPE_FILE) return -9;

    ext2_inode_t inode;
    if (ext2_read_inode(f->ino, &inode) != 0) return -5;

    fill_stat64(f->ino, &inode, st);
    return 0;
}

/* SYS_GETDENTS64 (220): similar to getdents but with struct linux_dirent64.
 * struct linux_dirent64 { u64 d_ino; s64 d_off; u16 d_reclen; u8 d_type; char d_name[]; } */
static int32_t sys_getdents64(struct isr_regs *regs) {
    uint32_t fd    = regs->ebx;
    uint8_t *ubuf  = (uint8_t *)regs->ecx;
    uint32_t bufsz = regs->edx;

    if (!ubuf || bufsz < 24)
        return -22;

    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES) return -9;

    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED) || !(f->flags & FD_FLAG_READABLE))
        return -9;
    if (f->type != FD_TYPE_FILE) return -22;

    ext2_inode_t dir;
    if (ext2_read_inode(f->ino, &dir) != 0) return -5;
    if (!(dir.i_mode & EXT2_S_IFDIR)) return -20; /* -ENOTDIR */

    uint32_t dir_offset = f->offset;
    uint32_t buf_used = 0;

    while (dir_offset < dir.i_size) {
        uint8_t blk_buf[1024];
        uint32_t to_read = 1024;
        if (dir_offset + to_read > dir.i_size)
            to_read = dir.i_size - dir_offset;

        int rn = ext2_read_file(f->ino, blk_buf, dir_offset, to_read);
        if (rn <= 0) break;

        uint32_t blk_off = 0;
        while (blk_off < (uint32_t)rn) {
            ext2_dir_entry_t *de = (ext2_dir_entry_t *)(blk_buf + blk_off);
            if (de->rec_len == 0) goto done64;

            if (de->inode != 0) {
                /* linux_dirent64: u64 d_ino(8) + s64 d_off(8) + u16 d_reclen(2) + u8 d_type(1) + name + NUL */
                uint32_t name_len = de->name_len;
                uint32_t entry_size = 8 + 8 + 2 + 1 + name_len + 1;
                entry_size = (entry_size + 7) & ~7u; /* align to 8 */

                if (buf_used + entry_size > bufsz) goto done64;

                /* d_ino (u64) */
                *(uint64_t *)(ubuf + buf_used) = de->inode;
                /* d_off (s64) — use file offset after this entry */
                *(uint64_t *)(ubuf + buf_used + 8) = dir_offset + de->rec_len;
                /* d_reclen (u16) */
                *(uint16_t *)(ubuf + buf_used + 16) = (uint16_t)entry_size;
                /* d_type (u8) */
                ubuf[buf_used + 18] = de->file_type;
                /* d_name */
                memcpy(ubuf + buf_used + 19, de->name, name_len);
                ubuf[buf_used + 19 + name_len] = '\0';

                buf_used += entry_size;
            }

            dir_offset += de->rec_len;
            blk_off += de->rec_len;
        }
    }

done64:
    f->offset = dir_offset;
    return (int32_t)buf_used;
}

/* SYS_OPENAT (295): open relative to directory fd.
 *   ebx = dirfd (AT_FDCWD = -100 for cwd)
 *   ecx = path
 *   edx = flags
 *   esi = mode (for O_CREAT)
 *   Returns: fd or -errno. */
static int32_t sys_openat(struct isr_regs *regs) {
    int32_t  dirfd = (int32_t)regs->ebx;
    const char *path = (const char *)regs->ecx;
    uint32_t flags = regs->edx;

    if (!path) return -14;

    /* If path is absolute or dirfd is AT_FDCWD, use regular open */
    if (path[0] == '/' || dirfd == -100) {
        /* Reuse sys_open by shuffling registers */
        struct isr_regs fake = *regs;
        fake.ebx = (uint32_t)path;
        fake.ecx = flags;
        return sys_open(&fake);
    }

    /* Relative to a dirfd — for now just treat as relative to cwd */
    struct isr_regs fake = *regs;
    fake.ebx = (uint32_t)path;
    fake.ecx = flags;
    return sys_open(&fake);
}

/* SYS_FSTATAT64 (300): stat relative to dirfd.
 *   ebx = dirfd, ecx = path, edx = stat buf, esi = flags
 *   Returns: 0 or -errno. */
static int32_t sys_fstatat64(struct isr_regs *regs) {
    const char *path = (const char *)regs->ecx;
    struct linux_stat64 *st = (struct linux_stat64 *)regs->edx;

    if (!path || !st) return -14;

    /* For now: ignore dirfd, use absolute/cwd resolution */
    uint32_t ino = resolve_path(path);
    if (ino == 0) return -2;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) return -5;

    fill_stat64(ino, &inode, st);
    return 0;
}

/* SYS_FCNTL64 (221): same as fcntl for us (no large-file differences). */
static int32_t sys_fcntl64(struct isr_regs *regs) {
    return sys_fcntl(regs);
}

/* SYS_TIME (13): get time in seconds since epoch (boot for us).
 *   ebx = pointer to time_t (can be NULL)
 *   Returns: seconds. */
static int32_t sys_time(struct isr_regs *regs) {
    uint32_t secs = timer_get_ticks() / 1000;
    uint32_t *tp = (uint32_t *)regs->ebx;
    if (tp) *tp = secs;
    return (int32_t)secs;
}

/* SYS_READLINK (85): read symlink target.
 *   ebx = path, ecx = buf, edx = bufsiz
 *   Returns: -EINVAL (no symlinks supported). */
static int32_t sys_readlink(struct isr_regs *regs) {
    (void)regs;
    return -22; /* -EINVAL: not a symlink */
}

/* SYS_GETUID32 (199) etc: 32-bit versions of uid/gid syscalls */
static int32_t sys_getuid32(struct isr_regs *regs) { return sys_getuid(regs); }
static int32_t sys_getgid32(struct isr_regs *regs) { return sys_getgid(regs); }
static int32_t sys_geteuid32(struct isr_regs *regs) { return sys_geteuid(regs); }
static int32_t sys_getegid32(struct isr_regs *regs) { return sys_getegid(regs); }
static int32_t sys_setuid32(struct isr_regs *regs) { return sys_setuid(regs); }
static int32_t sys_setgid32(struct isr_regs *regs) { return sys_setgid(regs); }

/* SYS_GETTID (224): get thread ID (same as PID for single-threaded). */
static int32_t sys_gettid(struct isr_regs *regs) {
    return sys_getpid(regs);
}

/* SYS_CLONE (120): create child process/thread.
 *   For now, only support fork-like clone (flags = SIGCHLD).
 *   ebx = flags, ecx = child_stack (0 = copy parent), edx = ptid, esi = tls, edi = ctid
 *   Returns: child pid to parent, 0 to child, -errno. */
static int32_t sys_clone(struct isr_regs *regs) {
    uint32_t child_stack = regs->ecx;

    /* If a child stack is specified, temporarily override useresp so
     * sys_fork records the new stack as the child's ESP. */
    uint32_t saved_useresp = regs->useresp;
    if (child_stack)
        regs->useresp = child_stack;

    int32_t ret = sys_fork(regs);

    /* Restore parent's useresp so iret returns to the right stack */
    regs->useresp = saved_useresp;

    return ret;
}

/* SYS_WAIT4 (114): wait for child (superset of waitpid).
 *   ebx = pid, ecx = status ptr, edx = options, esi = rusage ptr
 *   Returns: child pid or -errno. */
static int32_t sys_wait4(struct isr_regs *regs) {
    /* rusage (esi) is ignored */
    /* options (edx) — WNOHANG=1 */
    uint32_t options = regs->edx;

    task_t *parent = get_current_task();
    if (!parent) return -22;

    int32_t wait_pid = (int32_t)regs->ebx;
    int *status = (int *)regs->ecx;

    if (options & 1) { /* WNOHANG */
        if (wait_pid > 0) {
            task_t *child = find_task_by_pid((uint32_t)wait_pid);
            if (!child || child->parent_pid != parent->pid) return -10; /* -ECHILD */
            if (child->state == TASK_ZOMBIE) {
                int32_t cpid = (int32_t)child->pid;
                if (status) *status = (child->exit_code & 0xFF) << 8;
                remove_task(child);
                return cpid;
            }
            return 0; /* not yet exited */
        } else {
            int found_any = 0;
            task_t *zombie = find_child_task(parent->pid, 1, &found_any);
            if (zombie) {
                int32_t cpid = (int32_t)zombie->pid;
                if (status) *status = (zombie->exit_code & 0xFF) << 8;
                remove_task(zombie);
                return cpid;
            }
            if (!found_any) return -10;
            return 0;
        }
    }

    /* Blocking wait — use existing waitpid logic */
    for (;;) {
        if (wait_pid > 0) {
            task_t *child = find_task_by_pid((uint32_t)wait_pid);
            if (!child || child->parent_pid != parent->pid) return -10;
            if (child->state == TASK_ZOMBIE) {
                int32_t cpid = (int32_t)child->pid;
                if (status) *status = (child->exit_code & 0xFF) << 8;
                remove_task(child);
                return cpid;
            }
        } else {
            int found_any = 0;
            task_t *zombie = find_child_task(parent->pid, 1, &found_any);
            if (zombie) {
                int32_t cpid = (int32_t)zombie->pid;
                if (status) *status = (zombie->exit_code & 0xFF) << 8;
                remove_task(zombie);
                return cpid;
            }
            if (!found_any) return -10;
        }
        block_task(parent);
    }
}

/* SYS_PIPE2 (331): pipe with flags.
 *   ebx = pipefd ptr, ecx = flags (O_CLOEXEC, O_NONBLOCK)
 *   Returns: 0 or -errno. */
static int32_t sys_pipe2(struct isr_regs *regs) {
    /* Ignore flags for now, just create a pipe */
    return sys_pipe(regs);
}

/* SYS_DUP3 (330): dup2 with flags.
 *   ebx = oldfd, ecx = newfd, edx = flags
 *   Returns: newfd or -errno. */
static int32_t sys_dup3(struct isr_regs *regs) {
    return sys_dup2(regs);
}

/* SYS_RMDIR (40): remove directory.
 *   ebx = path
 *   Returns: 0 or -errno. */
static int32_t sys_rmdir(struct isr_regs *regs) {
    const char *path = (const char *)regs->ebx;
    if (!path) return -14;

    uint32_t ino = resolve_path(path);
    if (ino == 0) return -2;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) return -5;
    if ((inode.i_mode & 0xF000) != EXT2_S_IFDIR) return -20;

    /* Find parent and name */
    const char *last_slash = NULL;
    for (const char *p = path; *p; p++)
        if (*p == '/') last_slash = p;

    uint32_t parent_ino;
    const char *name;
    if (last_slash) {
        char pp[256];
        int plen = (int)(last_slash - path);
        if (plen == 0) parent_ino = EXT2_ROOT_INO;
        else {
            if (plen > 255) plen = 255;
            memcpy(pp, path, plen); pp[plen] = '\0';
            parent_ino = resolve_path(pp);
        }
        name = last_slash + 1;
    } else {
        task_t *t = get_current_task();
        parent_ino = t ? t->cwd_ino : EXT2_ROOT_INO;
        name = path;
    }

    if (parent_ino == 0) return -2;
    return ext2_remove(parent_ino, name);
}

/* SYS_LINK (9): hard link (stub — not supported). */
static int32_t sys_link(struct isr_regs *regs) {
    (void)regs;
    return -38; /* -ENOSYS */
}

/* SYS_SETPGID (57): set process group ID.
 *   ebx = pid (0 = self), ecx = pgid (0 = use pid)
 *   Returns: 0 (stub). */
static int32_t sys_setpgid(struct isr_regs *regs) {
    (void)regs;
    return 0; /* stub: accept silently */
}

/* SYS_GETPGRP (65): get process group.
 *   Returns: pid (stub: each process is its own group). */
static int32_t sys_getpgrp(struct isr_regs *regs) {
    return sys_getpid(regs);
}

/* SYS_SETSID (66): create a new session.
 *   Returns: new session ID = pid (stub). */
static int32_t sys_setsid(struct isr_regs *regs) {
    return sys_getpid(regs);
}

/* SYS_UNLINKAT (301): unlink relative to dirfd.
 *   ebx = dirfd, ecx = path, edx = flags (AT_REMOVEDIR)
 *   For now just delegate to unlink/rmdir. */
static int32_t sys_unlinkat(struct isr_regs *regs) {
    uint32_t flags = regs->edx;
    struct isr_regs fake = *regs;
    fake.ebx = regs->ecx; /* path */
    if (flags & 0x200) /* AT_REMOVEDIR */
        return sys_rmdir(&fake);
    return sys_unlink(&fake);
}

/* SYS_RENAMEAT (302): rename relative to dirfds.
 *   ebx = olddirfd, ecx = oldpath, edx = newdirfd, esi = newpath */
static int32_t sys_renameat(struct isr_regs *regs) {
    struct isr_regs fake = *regs;
    fake.ebx = regs->ecx; /* old path */
    fake.ecx = regs->esi; /* new path */
    return sys_rename(&fake);
}

/* SYS_FCHMOD (94) / SYS_FCHMODAT (306) */
static int32_t sys_fchmod(struct isr_regs *regs) {
    uint32_t fd = regs->ebx;
    uint16_t mode = (uint16_t)(regs->ecx & 0xFFF);

    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES) return -9;
    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED) || f->type != FD_TYPE_FILE) return -9;

    ext2_inode_t inode;
    if (ext2_read_inode(f->ino, &inode) != 0) return -5;
    if (t->euid != 0 && t->euid != inode.i_uid) return -1; /* -EPERM */
    inode.i_mode = (inode.i_mode & 0xF000) | mode;
    if (ext2_write_inode(f->ino, &inode) != 0) return -5;
    return 0;
}

static int32_t sys_fchmodat(struct isr_regs *regs) {
    /* ebx = dirfd, ecx = path, edx = mode */
    struct isr_regs fake = *regs;
    fake.ebx = regs->ecx;
    fake.ecx = regs->edx;
    return sys_chmod(&fake);
}

/* SYS_FCHOWN (95) / SYS_FCHOWNAT (298) */
static int32_t sys_fchown(struct isr_regs *regs) {
    uint32_t fd = regs->ebx;
    task_t *t = get_current_task();
    if (!t || fd >= MAX_OPEN_FILES) return -9;
    if (t->euid != 0) return -1;
    /* Accept but NOP for now */
    return 0;
}

static int32_t sys_fchownat(struct isr_regs *regs) {
    struct isr_regs fake = *regs;
    fake.ebx = regs->ecx;
    fake.ecx = regs->edx;
    fake.edx = regs->esi;
    return sys_chown(&fake);
}

/* SYS_CHOWN32 (212) */
static int32_t sys_chown32(struct isr_regs *regs) {
    return sys_chown(regs);
}

/* ------------------------------------------------------------------ */
/* SYS_GETTIMEOFDAY (78): get time of day                              */
/* ------------------------------------------------------------------ */
static int32_t sys_gettimeofday(struct isr_regs *regs) {
    struct k_timeval { uint32_t tv_sec; uint32_t tv_usec; };
    struct k_timezone { int tz_minuteswest; int tz_dsttime; };

    struct k_timeval *tv = (struct k_timeval *)regs->ebx;
    struct k_timezone *tz = (struct k_timezone *)regs->ecx;

    uint32_t ticks = timer_get_ticks();
    if (tv) {
        tv->tv_sec  = ticks / 1000;
        tv->tv_usec = (ticks % 1000) * 1000;
    }
    if (tz) {
        tz->tz_minuteswest = 0;
        tz->tz_dsttime     = 0;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* SYS_UNAME (122): system identification                              */
/* ------------------------------------------------------------------ */
static int32_t sys_uname(struct isr_regs *regs) {
    struct new_utsname {
        char sysname[65];
        char nodename[65];
        char release[65];
        char version[65];
        char machine[65];
        char domainname[65];
    } *buf = (void *)regs->ebx;

    if (!buf) return -14;

    memset(buf, 0, sizeof(*buf));
    /* manual copy to avoid pulling in strncpy */
    { const char *s; char *d; size_t n;
      s="MOSS";  d=buf->sysname;  n=sizeof(buf->sysname)-1;  while(n-- && *s) *d++=*s++;
      s="moss";  d=buf->nodename; n=sizeof(buf->nodename)-1; while(n-- && *s) *d++=*s++;
      s="0.1.0"; d=buf->release;  n=sizeof(buf->release)-1;  while(n-- && *s) *d++=*s++;
      s="#1";    d=buf->version;  n=sizeof(buf->version)-1;  while(n-- && *s) *d++=*s++;
      s="i686";  d=buf->machine;  n=sizeof(buf->machine)-1;  while(n-- && *s) *d++=*s++;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* SYS_POLL (168)                                                      */
/* ------------------------------------------------------------------ */

#define POLLIN   0x001
#define POLLOUT  0x004
#define POLLERR  0x008
#define POLLHUP  0x010
#define POLLNVAL 0x020

struct pollfd {
    int   fd;
    short events;
    short revents;
};

/* Forward declarations (defined below in the select section) */
static int fd_readable(task_t *t, int fd);
static int fd_writable(task_t *t, int fd);

static int32_t sys_poll(struct isr_regs *regs) {
    struct pollfd *fds = (struct pollfd *)regs->ebx;
    uint32_t nfds      = regs->ecx;
    int32_t timeout_ms = (int32_t)regs->edx;

    if (!fds && nfds > 0) return -14;

    task_t *t = get_current_task();
    if (!t) return -1;

    uint32_t tmo = (timeout_ms < 0) ? 0xFFFFFFFFu : (uint32_t)timeout_ms;
    uint32_t start = timer_get_ticks();

    while (1) {
        int ready = 0;
        for (uint32_t i = 0; i < nfds; i++) {
            fds[i].revents = 0;
            int fd = fds[i].fd;
            if (fd < 0) continue;
            if (fd >= MAX_OPEN_FILES || !(t->fd_table[fd].flags & FD_FLAG_USED)) {
                fds[i].revents = POLLNVAL;
                ready++;
                continue;
            }
            if ((fds[i].events & POLLIN) && fd_readable(t, fd)) {
                fds[i].revents |= POLLIN;
                ready++;
            }
            if ((fds[i].events & POLLOUT) && fd_writable(t, fd)) {
                fds[i].revents |= POLLOUT;
                ready++;
            }
        }
        if (ready > 0)
            return ready;
        if (tmo == 0)
            return 0;
        uint32_t elapsed = timer_get_ticks() - start;
        if (tmo != 0xFFFFFFFFu && elapsed >= tmo)
            return 0;
        t->state = TASK_INTERRUPTIBLE;
        schedule();
        if (t->sig_pending & ~t->sig_blocked)
            return -4; /* -EINTR */
    }
}

/* ------------------------------------------------------------------ */
/* SYS_SELECT (82) / SYS_NEWSELECT (142) / SYS_PSELECT6 (308)         */
/*   Check fd readiness for reading/writing + timeout support          */
/* ------------------------------------------------------------------ */

/* Check if a file descriptor is ready for reading */
static int fd_readable(task_t *t, int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return 0;
    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED)) return 0;
    if (!(f->flags & FD_FLAG_READABLE)) return 0;

    switch (f->type) {
    case FD_TYPE_STDIN:
    case FD_TYPE_DEVTTY:
        /* Readable if cooked data available, EOF pending, or raw
         * keyboard events queued (so select() doesn't block when
         * no one has called read() to cook them yet). */
        {
            extern int keyboard_has_events(void);
            return (tty_ldisc.cooked_count > 0 ||
                    tty_ldisc.eof_pending ||
                    keyboard_has_events());
        }
    case FD_TYPE_PIPE: {
        pipe_t *p = &pipe_table[f->pipe_idx];
        /* Readable if data available or all writers closed (EOF) */
        return (p->count > 0 || p->writers == 0);
    }
    case FD_TYPE_FILE:
        /* Regular files are always ready */
        return 1;
    case FD_TYPE_DEVNULL:
        /* /dev/null always ready (returns EOF) */
        return 1;
    default:
        return 0;
    }
}

/* Check if a file descriptor is ready for writing */
static int fd_writable(task_t *t, int fd) {
    if (fd < 0 || fd >= MAX_OPEN_FILES) return 0;
    fd_entry_t *f = &t->fd_table[fd];
    if (!(f->flags & FD_FLAG_USED)) return 0;
    if (!(f->flags & FD_FLAG_WRITABLE)) return 0;

    switch (f->type) {
    case FD_TYPE_STDOUT:
    case FD_TYPE_STDERR:
    case FD_TYPE_DEVTTY:
    case FD_TYPE_DEVNULL:
    case FD_TYPE_FILE:
        /* Always writable */
        return 1;
    case FD_TYPE_PIPE: {
        pipe_t *p = &pipe_table[f->pipe_idx];
        /* Writable if buffer has space or no readers (SIGPIPE on write) */
        return (p->count < PIPE_BUF_SIZE || p->readers == 0);
    }
    default:
        return 0;
    }
}

/* Poll all fds in the bitmask sets, return number ready */
static int do_select_poll(task_t *t, int nfds,
                          uint32_t *readfds, uint32_t *writefds,
                          uint32_t *exceptfds) {
    int ready = 0;
    int nwords = (nfds + 31) / 32;

    /* Save input sets, then clear output */
    uint32_t in_read[nwords], in_write[nwords], in_except[nwords];
    if (readfds)   memcpy(in_read,   readfds,   nwords * 4);
    else           memset(in_read,   0, nwords * 4);
    if (writefds)  memcpy(in_write,  writefds,  nwords * 4);
    else           memset(in_write,  0, nwords * 4);
    if (exceptfds) memcpy(in_except, exceptfds, nwords * 4);
    else           memset(in_except, 0, nwords * 4);

    if (readfds)   memset(readfds,   0, nwords * 4);
    if (writefds)  memset(writefds,  0, nwords * 4);
    if (exceptfds) memset(exceptfds, 0, nwords * 4);

    for (int fd = 0; fd < nfds; fd++) {
        int word = fd / 32;
        uint32_t bit = 1u << (fd % 32);

        if (in_read[word] & bit) {
            if (fd_readable(t, fd)) {
                if (readfds) readfds[word] |= bit;
                ready++;
            }
        }
        if (in_write[word] & bit) {
            if (fd_writable(t, fd)) {
                if (writefds) writefds[word] |= bit;
                ready++;
            }
        }
        /* No exception conditions for now */
        (void)in_except;
    }
    return ready;
}

static int32_t sys_select(struct isr_regs *regs) {
    /* old select passes args via pointer in ebx */
    struct sel_args { int nfds; uint32_t *readfds; uint32_t *writefds;
                      uint32_t *exceptfds; void *timeout; };
    struct sel_args *a = (struct sel_args *)regs->ebx;
    if (!a) return -14;

    struct k_timeval { uint32_t tv_sec; uint32_t tv_usec; };
    struct k_timeval *tv = (struct k_timeval *)a->timeout;
    task_t *t = get_current_task();
    if (!t) return -1;

    uint32_t timeout_ms = 0xFFFFFFFFu; /* infinite if no timeout */
    if (tv)
        timeout_ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;

    uint32_t start = timer_get_ticks();
    while (1) {
        int ready = do_select_poll(t, a->nfds, a->readfds, a->writefds,
                                   a->exceptfds);
        if (ready > 0) {
            if (tv) { tv->tv_sec = 0; tv->tv_usec = 0; }
            return ready;
        }
        /* Check timeout */
        uint32_t elapsed = timer_get_ticks() - start;
        if (elapsed >= timeout_ms) {
            if (tv) { tv->tv_sec = 0; tv->tv_usec = 0; }
            return 0;
        }
        /* Yield and try again */
        t->state = TASK_INTERRUPTIBLE;
        schedule();
        /* Check for pending signals */
        if (t->sig_pending & ~t->sig_blocked)
            return -4; /* -EINTR */
    }
}

static int32_t sys_newselect(struct isr_regs *regs) {
    int nfds = (int)regs->ebx;
    uint32_t *readfds   = (uint32_t *)regs->ecx;
    uint32_t *writefds  = (uint32_t *)regs->edx;
    uint32_t *exceptfds = (uint32_t *)regs->esi;
    struct k_timeval { uint32_t tv_sec; uint32_t tv_usec; };
    struct k_timeval *tv = (struct k_timeval *)regs->edi;

    task_t *t = get_current_task();
    if (!t) return -1;

    uint32_t timeout_ms = 0xFFFFFFFFu;
    if (tv)
        timeout_ms = tv->tv_sec * 1000 + tv->tv_usec / 1000;

    uint32_t start = timer_get_ticks();
    while (1) {
        int ready = do_select_poll(t, nfds, readfds, writefds, exceptfds);
        if (ready > 0) {
            if (tv) { tv->tv_sec = 0; tv->tv_usec = 0; }
            return ready;
        }
        uint32_t elapsed = timer_get_ticks() - start;
        if (elapsed >= timeout_ms) {
            if (tv) { tv->tv_sec = 0; tv->tv_usec = 0; }
            return 0;
        }
        t->state = TASK_INTERRUPTIBLE;
        schedule();
        if (t->sig_pending & ~t->sig_blocked)
            return -4;
    }
}

static int32_t sys_pselect6(struct isr_regs *regs) {
    int nfds = (int)regs->ebx;
    uint32_t *readfds   = (uint32_t *)regs->ecx;
    uint32_t *writefds  = (uint32_t *)regs->edx;
    uint32_t *exceptfds = (uint32_t *)regs->esi;
    struct { uint32_t tv_sec; uint32_t tv_nsec; } *ts = (void *)regs->edi;

    task_t *t = get_current_task();
    if (!t) return -1;

    uint32_t timeout_ms = 0xFFFFFFFFu;
    if (ts)
        timeout_ms = ts->tv_sec * 1000 + ts->tv_nsec / 1000000;

    uint32_t start = timer_get_ticks();
    while (1) {
        int ready = do_select_poll(t, nfds, readfds, writefds, exceptfds);
        if (ready > 0)
            return ready;
        uint32_t elapsed = timer_get_ticks() - start;
        if (elapsed >= timeout_ms)
            return 0;
        t->state = TASK_INTERRUPTIBLE;
        schedule();
        if (t->sig_pending & ~t->sig_blocked)
            return -4;
    }
}

/* ------------------------------------------------------------------ */
/* SYS_GETRLIMIT (76) / SYS_UGETRLIMIT (191) / SYS_SETRLIMIT (75)     */
/* ------------------------------------------------------------------ */
#define RLIMIT_NOFILE   7
#define RLIMIT_STACK    3
#define RLIMIT_NPROC    6
#define RLIMIT_AS       9
#define RLIM_INFINITY   0xFFFFFFFFu

static int32_t sys_getrlimit(struct isr_regs *regs) {
    uint32_t resource = regs->ebx;
    struct { uint32_t rlim_cur; uint32_t rlim_max; } *rlim = (void *)regs->ecx;
    if (!rlim) return -14;

    switch (resource) {
        case RLIMIT_NOFILE:
            rlim->rlim_cur = MAX_OPEN_FILES;
            rlim->rlim_max = MAX_OPEN_FILES;
            break;
        case RLIMIT_STACK:
            rlim->rlim_cur = 8 * 1024 * 1024;  /* 8 MB */
            rlim->rlim_max = 8 * 1024 * 1024;
            break;
        default:
            rlim->rlim_cur = RLIM_INFINITY;
            rlim->rlim_max = RLIM_INFINITY;
            break;
    }
    return 0;
}

static int32_t sys_setrlimit(struct isr_regs *regs) {
    (void)regs;
    return 0; /* accept silently */
}

/* ------------------------------------------------------------------ */
/* SYS_GETRUSAGE (77): resource usage stats                            */
/* ------------------------------------------------------------------ */
static int32_t sys_getrusage(struct isr_regs *regs) {
    /* struct rusage is 72 bytes on i386. Zero it out. */
    void *buf = (void *)regs->ecx;
    if (!buf) return -14;
    memset(buf, 0, 72);
    return 0;
}

/* ------------------------------------------------------------------ */
/* SYS_SETITIMER (104) / SYS_GETITIMER (105) / SYS_ALARM (27)         */
/* ------------------------------------------------------------------ */
static uint32_t alarm_target_tick = 0;  /* 0 = no alarm pending */

static int32_t sys_alarm(struct isr_regs *regs) {
    uint32_t seconds = regs->ebx;
    uint32_t remaining = 0;
    uint32_t now = timer_get_ticks();

    if (alarm_target_tick && alarm_target_tick > now) {
        remaining = (alarm_target_tick - now + 999) / 1000;
    }

    if (seconds == 0)
        alarm_target_tick = 0;
    else
        alarm_target_tick = now + seconds * 1000;

    return (int32_t)remaining;
}

static int32_t sys_setitimer(struct isr_regs *regs) {
    uint32_t which = regs->ebx;
    struct k_itimerval {
        struct { uint32_t tv_sec; uint32_t tv_usec; } it_interval;
        struct { uint32_t tv_sec; uint32_t tv_usec; } it_value;
    };
    struct k_itimerval *nval = (struct k_itimerval *)regs->ecx;
    struct k_itimerval *oval = (struct k_itimerval *)regs->edx;

    (void)which; /* only ITIMER_REAL(0) makes sense for us */

    if (oval) {
        uint32_t now = timer_get_ticks();
        memset(oval, 0, sizeof(*oval));
        if (alarm_target_tick > now) {
            uint32_t rem_ms = alarm_target_tick - now;
            oval->it_value.tv_sec  = rem_ms / 1000;
            oval->it_value.tv_usec = (rem_ms % 1000) * 1000;
        }
    }
    if (nval) {
        uint32_t ms = nval->it_value.tv_sec * 1000 +
                      nval->it_value.tv_usec / 1000;
        if (ms == 0)
            alarm_target_tick = 0;
        else
            alarm_target_tick = timer_get_ticks() + ms;
    }
    return 0;
}

static int32_t sys_getitimer(struct isr_regs *regs) {
    struct k_itimerval {
        struct { uint32_t tv_sec; uint32_t tv_usec; } it_interval;
        struct { uint32_t tv_sec; uint32_t tv_usec; } it_value;
    };
    struct k_itimerval *oval = (struct k_itimerval *)regs->ecx;
    if (!oval) return -14;

    uint32_t now = timer_get_ticks();
    memset(oval, 0, sizeof(*oval));
    if (alarm_target_tick > now) {
        uint32_t rem_ms = alarm_target_tick - now;
        oval->it_value.tv_sec  = rem_ms / 1000;
        oval->it_value.tv_usec = (rem_ms % 1000) * 1000;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* SYS_TIMES (43): process times                                       */
/* ------------------------------------------------------------------ */
static int32_t sys_times(struct isr_regs *regs) {
    struct tms {
        uint32_t tms_utime;
        uint32_t tms_stime;
        uint32_t tms_cutime;
        uint32_t tms_cstime;
    } *buf = (void *)regs->ebx;
    uint32_t ticks = timer_get_ticks();
    if (buf) {
        buf->tms_utime  = ticks;
        buf->tms_stime  = 0;
        buf->tms_cutime = 0;
        buf->tms_cstime = 0;
    }
    return (int32_t)ticks;
}

/* ------------------------------------------------------------------ */
/* SYS_SIGALTSTACK (186): alternate signal stack (stub)                */
/* ------------------------------------------------------------------ */
static int32_t sys_sigaltstack(struct isr_regs *regs) {
    (void)regs;
    return 0; /* accept silently, we don't use altstack */
}

/* ------------------------------------------------------------------ */
/* SYS_RT_SIGSUSPEND (179): wait for a signal                          */
/* ------------------------------------------------------------------ */
static int32_t sys_rt_sigsuspend(struct isr_regs *regs) {
    (void)regs;
    /* Just yield once — real implementation would block until signal */
    task_t *me = get_current_task();
    if (me) {
        me->state = TASK_INTERRUPTIBLE;
        schedule();
    }
    return -4; /* -EINTR */
}

/* ------------------------------------------------------------------ */
/* SYS_SYSINFO (116): system info (stub)                               */
/* ------------------------------------------------------------------ */
static int32_t sys_sysinfo(struct isr_regs *regs) {
    struct k_sysinfo {
        uint32_t uptime;
        uint32_t loads[3];
        uint32_t totalram;
        uint32_t freeram;
        uint32_t sharedram;
        uint32_t bufferram;
        uint32_t totalswap;
        uint32_t freeswap;
        uint16_t procs;
        uint16_t pad;
        uint32_t totalhigh;
        uint32_t freehigh;
        uint32_t mem_unit;
        char _pad[8];
    } *info = (void *)regs->ebx;
    if (!info) return -14;
    memset(info, 0, sizeof(*info));
    info->uptime   = timer_get_ticks() / 1000;
    info->totalram = 64 * 1024 * 1024; /* 64 MB placeholder */
    info->freeram  = 32 * 1024 * 1024;
    info->procs    = 8;
    info->mem_unit = 1;
    return 0;
}

/* ------------------------------------------------------------------ */
/* SYS_MKNOD (14): create special file (stub — just files for now)     */
/* ------------------------------------------------------------------ */
static int32_t sys_mknod(struct isr_regs *regs) {
    (void)regs;
    /* mkfifo calls mknod with S_IFIFO mode. MOSS doesn't support FIFOs on disk. */
    return -38; /* -ENOSYS */
}

/* ------------------------------------------------------------------ */
/* SYS_GETPGID (132): get process group for any pid                    */
/* ------------------------------------------------------------------ */
static int32_t sys_getpgid(struct isr_regs *regs) {
    (void)regs;
    /* No real process groups: return caller PID */
    task_t *me = get_current_task();
    return me ? (int32_t)me->pid : -3;
}

/* ------------------------------------------------------------------ */
/* SYS_SETREUID32 (203) / SYS_SETREGID32 (204) — stubs                */
/* ------------------------------------------------------------------ */
static int32_t sys_setreuid32(struct isr_regs *regs) { (void)regs; return 0; }
static int32_t sys_setregid32(struct isr_regs *regs) { (void)regs; return 0; }
static int32_t sys_getgroups32(struct isr_regs *regs) { (void)regs; return 0; }
static int32_t sys_setgroups32(struct isr_regs *regs) { (void)regs; return 0; }
static int32_t sys_setresuid32(struct isr_regs *regs) { (void)regs; return 0; }
static int32_t sys_setresgid32(struct isr_regs *regs) { (void)regs; return 0; }

/* SYS_FACCESSAT (307): check file access relative to a directory fd.
 *   ebx = dirfd (AT_FDCWD = -100), ecx = path, edx = mode
 *   Returns: 0 on success, -errno on error. */
static int32_t sys_faccessat(struct isr_regs *regs) {
    /* Reuse sys_access logic: ignore dirfd, resolve path directly */
    const char *path = (const char *)regs->ecx;
    uint32_t mode    = regs->edx;

    if (!path)
        return -14; /* -EFAULT */

    uint32_t ino = resolve_path(path);
    if (ino == 0)
        return -2; /* -ENOENT */

    if (mode == 0) /* F_OK */
        return 0;

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -2;

    task_t *t = get_current_task();
    uint16_t uid = t ? t->uid : 0;
    uint16_t gid = t ? t->gid : 0;

    if (!check_permission(uid, gid, inode.i_uid, inode.i_gid,
                          inode.i_mode & 0xFFF, (int)mode))
        return -13; /* -EACCES */

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Rebuilt syscall table — Linux i386 numbering                       */
/* ------------------------------------------------------------------ */

static syscall_fn_t syscall_table[NUM_SYSCALLS];

static void syscall_table_init(void) {
    memset(syscall_table, 0, sizeof(syscall_table));

    syscall_table[SYS_EXIT]            = sys_exit;          /* 1 */
    syscall_table[SYS_FORK]            = sys_fork;          /* 2 */
    syscall_table[SYS_READ]            = sys_read;          /* 3 */
    syscall_table[SYS_WRITE]           = sys_write;         /* 4 */
    syscall_table[SYS_OPEN]            = sys_open;          /* 5 */
    syscall_table[SYS_CLOSE]           = sys_close;         /* 6 */
    syscall_table[SYS_WAITPID]         = sys_waitpid;       /* 7 */
    syscall_table[SYS_LINK]            = sys_link;          /* 9 */
    syscall_table[SYS_UNLINK]          = sys_unlink;        /* 10 */
    syscall_table[SYS_EXECVE]          = sys_execve;        /* 11 */
    syscall_table[SYS_CHDIR]           = sys_chdir;         /* 12 */
    syscall_table[SYS_TIME]            = sys_time;          /* 13 */
    syscall_table[SYS_MKNOD]           = sys_mknod;         /* 14 */
    syscall_table[SYS_CHMOD]           = sys_chmod;         /* 15 */
    syscall_table[SYS_LSEEK]           = sys_lseek;         /* 19 */
    syscall_table[SYS_GETPID]          = sys_getpid;        /* 20 */
    syscall_table[SYS_SETUID]          = sys_setuid;        /* 23 */
    syscall_table[SYS_GETUID]          = sys_getuid;        /* 24 */
    syscall_table[SYS_ALARM]           = sys_alarm;         /* 27 */
    syscall_table[SYS_ACCESS]          = sys_access;        /* 33 */
    syscall_table[SYS_KILL]            = sys_kill;          /* 37 */
    syscall_table[SYS_RENAME]          = sys_rename;        /* 38 */
    syscall_table[SYS_MKDIR]           = sys_mkdir;         /* 39 */
    syscall_table[SYS_RMDIR]           = sys_rmdir;         /* 40 */
    syscall_table[SYS_DUP]             = sys_dup;           /* 41 */
    syscall_table[SYS_PIPE]            = sys_pipe;          /* 42 */
    syscall_table[SYS_TIMES]           = sys_times;         /* 43 */
    syscall_table[SYS_BRK]             = sys_brk;           /* 45 */
    syscall_table[SYS_SETGID]          = sys_setgid;        /* 46 */
    syscall_table[SYS_GETGID]          = sys_getgid;        /* 47 */
    syscall_table[SYS_SIGNAL]          = sys_signal;        /* 48 */
    syscall_table[SYS_GETEUID]         = sys_geteuid;       /* 49 */
    syscall_table[SYS_GETEGID]         = sys_getegid;       /* 50 */
    syscall_table[SYS_IOCTL]           = sys_ioctl;         /* 54 */
    syscall_table[SYS_FCNTL]           = sys_fcntl;         /* 55 */
    syscall_table[SYS_SETPGID]         = sys_setpgid;       /* 57 */
    syscall_table[SYS_UMASK]           = sys_umask;         /* 60 */
    syscall_table[SYS_DUP2]            = sys_dup2;          /* 63 */
    syscall_table[SYS_GETPPID]         = sys_getppid;       /* 64 */
    syscall_table[SYS_GETPGRP]         = sys_getpgrp;       /* 65 */
    syscall_table[SYS_SETSID]          = sys_setsid;        /* 66 */
    syscall_table[SYS_SIGACTION]       = sys_signal;        /* 67 (old sigaction) */
    syscall_table[SYS_SETRLIMIT]       = sys_setrlimit;     /* 75 */
    syscall_table[SYS_GETRLIMIT]       = sys_getrlimit;     /* 76 */
    syscall_table[SYS_GETRUSAGE]       = sys_getrusage;     /* 77 */
    syscall_table[SYS_GETTIMEOFDAY]    = sys_gettimeofday;  /* 78 */
    syscall_table[SYS_SELECT]          = sys_select;        /* 82 */
    syscall_table[SYS_READLINK]        = sys_readlink;      /* 85 */
    syscall_table[SYS_MMAP]            = sys_old_mmap;      /* 90 (old mmap) */
    syscall_table[SYS_MUNMAP]          = sys_munmap;        /* 91 */
    syscall_table[SYS_SETITIMER]       = sys_setitimer;     /* 104 */
    syscall_table[SYS_GETITIMER]       = sys_getitimer;     /* 105 */
    syscall_table[SYS_FCHMOD]          = sys_fchmod;        /* 94 */
    syscall_table[SYS_FCHOWN]          = sys_fchown;        /* 95 */
    syscall_table[SYS_STAT]            = sys_stat;          /* 106 (old stat) */
    syscall_table[SYS_FSTAT]           = sys_fstat;         /* 108 (old fstat) */
    syscall_table[SYS_SYSINFO]         = sys_sysinfo;       /* 116 */
    syscall_table[SYS_WAIT4]           = sys_wait4;         /* 114 */
    syscall_table[SYS_CLONE]           = sys_clone;         /* 120 */
    syscall_table[SYS_UNAME]           = sys_uname;         /* 122 */
    syscall_table[SYS_MPROTECT]        = sys_mprotect;      /* 125 */
    syscall_table[SYS_GETPGID]         = sys_getpgid;       /* 132 */
    syscall_table[SYS_NEWSELECT]       = sys_newselect;     /* 142 */
    syscall_table[SYS_WRITEV]          = sys_writev;        /* 146 */
    syscall_table[SYS_NANOSLEEP]       = sys_nanosleep;     /* 162 */
    syscall_table[SYS_POLL]            = sys_poll;          /* 168 */
    syscall_table[141]                 = sys_getdents;      /* old getdents */
    syscall_table[SYS_RT_SIGRETURN]    = sys_rt_sigreturn;  /* 173 */
    syscall_table[SYS_RT_SIGACTION]    = sys_rt_sigaction;  /* 174 */
    syscall_table[SYS_RT_SIGPROCMASK]  = sys_rt_sigprocmask;/* 175 */
    syscall_table[SYS_RT_SIGSUSPEND]   = sys_rt_sigsuspend; /* 179 */
    syscall_table[SYS_GETCWD]          = sys_getcwd;        /* 183 */
    syscall_table[SYS_SIGALTSTACK]     = sys_sigaltstack;   /* 186 */
    syscall_table[SYS_UGETRLIMIT]      = sys_getrlimit;     /* 191 (same as 76) */
    syscall_table[SYS_MMAP2]           = sys_mmap2;         /* 192 */
    syscall_table[SYS_STAT64]          = sys_stat64;        /* 195 */
    syscall_table[SYS_LSTAT64]         = sys_lstat64;       /* 196 */
    syscall_table[SYS_FSTAT64]         = sys_fstat64;       /* 197 */
    syscall_table[SYS_GETUID32]        = sys_getuid32;      /* 199 */
    syscall_table[SYS_GETGID32]        = sys_getgid32;      /* 200 */
    syscall_table[SYS_GETEUID32]       = sys_geteuid32;     /* 201 */
    syscall_table[SYS_GETEGID32]       = sys_getegid32;     /* 202 */
    syscall_table[SYS_SETREUID32]      = sys_setreuid32;    /* 203 */
    syscall_table[SYS_SETREGID32]      = sys_setregid32;    /* 204 */
    syscall_table[SYS_GETGROUPS32]     = sys_getgroups32;   /* 205 */
    syscall_table[SYS_SETGROUPS32]     = sys_setgroups32;   /* 206 */
    syscall_table[SYS_SETRESUID32]     = sys_setresuid32;   /* 208 */
    syscall_table[SYS_SETRESGID32]     = sys_setresgid32;   /* 210 */
    syscall_table[SYS_CHOWN32]         = sys_chown32;       /* 212 */
    syscall_table[SYS_SETUID32]        = sys_setuid32;      /* 213 */
    syscall_table[SYS_SETGID32]        = sys_setgid32;      /* 214 */
    syscall_table[SYS_GETDENTS64]      = sys_getdents64;    /* 220 */
    syscall_table[SYS_FCNTL64]         = sys_fcntl64;       /* 221 */
    syscall_table[SYS_GETTID]          = sys_gettid;        /* 224 */
    syscall_table[SYS_SET_THREAD_AREA] = sys_set_thread_area;/* 243 */
    syscall_table[SYS_EXIT_GROUP]      = sys_exit_group;    /* 252 */
    syscall_table[SYS_CLOCK_GETTIME]   = sys_clock_gettime; /* 265 */
    syscall_table[SYS_OPENAT]          = sys_openat;        /* 295 */
    syscall_table[SYS_FCHOWNAT]        = sys_fchownat;      /* 298 */
    syscall_table[SYS_PSELECT6]        = sys_pselect6;      /* 308 */
    syscall_table[SYS_FSTATAT64]       = sys_fstatat64;     /* 300 */
    syscall_table[SYS_UNLINKAT]        = sys_unlinkat;      /* 301 */
    syscall_table[SYS_RENAMEAT]        = sys_renameat;      /* 302 */
    syscall_table[SYS_FCHMODAT]        = sys_fchmodat;      /* 306 */
    syscall_table[SYS_FACCESSAT]       = sys_faccessat;     /* 307 */
    syscall_table[SYS_DUP3]            = sys_dup3;          /* 330 */
    syscall_table[SYS_PIPE2]           = sys_pipe2;         /* 331 */
    syscall_table[SYS_CLOCK_GETTIME64] = sys_clock_gettime64;/* 403 */

    /* MOSS-specific syscalls (500+) */
    syscall_table[SYS_FBMAP]           = sys_fbmap;         /* 500 */
    syscall_table[SYS_GETTICKS]        = sys_getticks;      /* 501 */
    syscall_table[SYS_WRITE_N]         = sys_write_n;       /* 502 */
    syscall_table[SYS_POLL_KEY]        = sys_poll_key;      /* 503 */
    syscall_table[SYS_WAIT_KEY]        = sys_wait_key;      /* 504 */
    syscall_table[SYS_GET_WINSIZE]     = sys_get_winsize;   /* 505 */
    syscall_table[SYS_FB_FLUSH]        = sys_fb_flush;      /* 506 */
    syscall_table[SYS_AUDIO_WRITE]     = sys_audio_write;   /* 507 */
    syscall_table[SYS_AUDIO_AVAIL]     = sys_audio_avail;   /* 508 */
    syscall_table[SYS_AUDIO_START]     = sys_audio_start;   /* 509 */
    syscall_table[SYS_ISATTY]          = sys_isatty;        /* 510 */
    syscall_table[SYS_SETENV]          = sys_setenv;        /* 511 */
    syscall_table[SYS_GETENV]          = sys_getenv;        /* 512 */
    syscall_table[SYS_POLL_MOUSE]       = sys_poll_mouse;    /* 513 */
    syscall_table[SYS_GET_MOUSE_POS]    = sys_get_mouse_pos; /* 514 */

    /* Suppress unused-function warnings for legacy syscall implementations */
    (void)sys_usleep;
    (void)sys_gettime;
    (void)sys_readv;
}

/* ------------------------------------------------------------------ */
/*  Dispatcher (called from the int 0x80 ISR stub)                     */
/* ------------------------------------------------------------------ */

/*
 * isr_regs layout (from idt.h):
 *   ds
 *   edi, esi, ebp, esp, ebx, edx, ecx, eax   (pushal order)
 *   int_no, err_code
 *   eip, cs, eflags, useresp, ss
 *
 * We read the syscall number from eax, dispatch, and write the
 * return value back into eax so popal restores it to the caller.
 */
static void syscall_dispatch(struct isr_regs *regs) {
    uint32_t num = regs->eax;

    if (num >= NUM_SYSCALLS || syscall_table[num] == NULL) {
        regs->eax = (uint32_t)-38;   /* -ENOSYS */
        return;
    }

    int32_t ret = syscall_table[num](regs);
    regs->eax = (uint32_t)ret;

    /* Check for pending signals before returning to user mode.
     * Don't deliver during sigreturn (we just restored context). */
    if (num != SYS_RT_SIGRETURN)
        deliver_signal(regs);
}

/* ------------------------------------------------------------------ */
/*  Initialisation                                                     */
/* ------------------------------------------------------------------ */

void syscall_init(void) {
    syscall_table_init();
    isr_register_handler(0x80, syscall_dispatch);
}
