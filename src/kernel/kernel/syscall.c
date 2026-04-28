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

/*
 * Register frame pushed by isr_common_stub (defined in arch/i386/idt.h).
 * Duplicated here so kernel/ code doesn't depend on arch/ include paths.
 */
struct isr_regs {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;  /* pushal */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;              /* pushed by CPU */
};

typedef void (*isr_handler_t)(struct isr_regs *regs);
void isr_register_handler(uint8_t n, isr_handler_t handler);

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

    /* If a parent task (shell) is waiting via elf_load_and_exec, unblock it */
    task_t *parent = elf_get_waiting_parent();
    if (parent)
        unblock_task(parent);

    /* Also try to unblock the parent if it's waiting via waitpid */
    if (current) {
        task_t *ptask = find_task_by_pid(current->parent_pid);
        if (ptask && ptask->state == TASK_BLOCKED && ptask != parent)
            unblock_task(ptask);
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
    if (f->type == FD_TYPE_STDOUT || f->type == FD_TYPE_STDERR) {
        terminal_write(buf, (size_t)count);
        return (int32_t)count;
    }

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

    /* stdin → keyboard */
    if (f->type == FD_TYPE_STDIN) {
        extern uint8_t get_key(void);
        uint8_t ch = get_key();
        buf[0] = (char)ch;
        return 1;
    }

    /* Regular file read */
    if (f->type == FD_TYPE_FILE) {
        int n = ext2_read_file(f->ino, buf, f->offset, count);
        if (n > 0)
            f->offset += (uint32_t)n;
        return n;
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
#define USER_STACK_BASE 0xBFFFB000u

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

    /* Don't allow the heap to grow into the user stack region */
    if (new_brk >= USER_STACK_BASE)
        return (int32_t)t->brk_current;

    uint32_t old_page = (t->brk_current + SYS_PAGE_SIZE - 1) & ~(SYS_PAGE_SIZE - 1);
    uint32_t new_page = (new_brk + SYS_PAGE_SIZE - 1) & ~(SYS_PAGE_SIZE - 1);

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

    /* Read the inode to get file size and verify it's a regular file */
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;

    if ((inode.i_mode & 0xF000) != EXT2_S_IFREG)
        return -1;  /* not a regular file */

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

    /* Don't allow closing stdin/stdout/stderr this way (optional, but safe) */

    f->type      = FD_TYPE_NONE;
    f->flags     = 0;
    f->ino       = 0;
    f->offset    = 0;
    f->file_size = 0;

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
 *   Returns: virtual address of mapped framebuffer, or 0 on error.
 *
 *   The framebuffer is identity-mapped in the kernel already.
 *   We just mark the existing pages as user-accessible. */
static int32_t sys_fbmap(struct isr_regs *regs) {
    user_fbinfo_t *uinfo = (user_fbinfo_t *)regs->ebx;
    framebuffer_info_t *fb = fb_get_info();
    if (!fb || !fb->address)
        return 0;

    uint32_t fb_phys  = (uint32_t)fb->address;
    uint32_t fb_size  = fb->pitch * fb->height;
    uint32_t fb_start = fb_phys & ~(SYS_PAGE_SIZE - 1);
    uint32_t fb_end   = (fb_phys + fb_size + SYS_PAGE_SIZE - 1) & ~(SYS_PAGE_SIZE - 1);

    /* Mark existing identity-mapped framebuffer pages as user-accessible */
    uint32_t *pd = paging_get_page_dir();
    for (uint32_t addr = fb_start; addr < fb_end; addr += SYS_PAGE_SIZE) {
        /* Re-map with user flag added */
        paging_map_page(addr, addr, PTE_PRESENT | PTE_WRITABLE | PTE_USER);
        pd[addr >> 22] |= PDE_USER;
    }

    /* Fill the info struct if provided */
    if (uinfo) {
        uinfo->address   = fb_phys;
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

    return (int32_t)fb_phys;
}

/* SYS_GETTICKS (9): return PIT tick count since boot.
 *   No arguments.
 *   Returns: tick count (100 Hz, so ticks/100 = seconds). */
static int32_t sys_getticks(struct isr_regs *regs) {
    (void)regs;
    return (int32_t)timer_get_ticks();
}

/* SYS_USLEEP (10): sleep for approximately N milliseconds.
 *   ebx = milliseconds to sleep
 *   Returns: 0. */
static int32_t sys_usleep(struct isr_regs *regs) {
    uint32_t ms = regs->ebx;
    /* Timer runs at 100 Hz → 10 ms per tick */
    uint32_t wait_ticks = (ms + 9) / 10;  /* round up */
    uint32_t start = timer_get_ticks();
    while (timer_get_ticks() - start < wait_ticks) {
        /* Yield the CPU: enable interrupts and halt until next tick */
        asm volatile("sti; hlt");
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
 *   Returns: seconds (ticks / 100). */
static int32_t sys_gettime(struct isr_regs *regs) {
    (void)regs;
    return (int32_t)(timer_get_ticks() / 100);
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
        nf->type = FD_TYPE_NONE;
        nf->flags = 0;
    }

    t->fd_table[new_fd] = *of;
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
        printf("fork_child_entry: get_current_task() returned NULL!\r\n");
        panic();
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

    printf("fork_child_entry: PID=%u  eip=0x%x  esp=0x%x  eflags=0x%x\r\n",
           self->pid, f_eip, f_esp, f_eflags);
    printf("  ebx=0x%x ecx=0x%x edx=0x%x esi=0x%x edi=0x%x ebp=0x%x\r\n",
           f_ebx, f_ecx, f_edx, f_esi, f_edi, f_ebp);
    printf("  page_dir=0x%x  slot=%d\r\n",
           (uint32_t)self->page_dir, self->user_pages_slot);

    /* Validate the saved user EIP looks sane (in user-space range) */
    if (f_eip < 0x08048000u || f_eip >= 0xC0000000u) {
        printf("fork_child_entry: INVALID eip 0x%x (not in user range)\r\n", f_eip);
        panic();
    }
    /* Validate user ESP is in the user stack region */
    if (f_esp < 0x08000000u || f_esp >= 0xC0000000u) {
        printf("fork_child_entry: INVALID esp 0x%x\r\n", f_esp);
        panic();
    }

    asm volatile(
        "cli\n\t"
        /* Load user data segment into all segment registers */
        "movw $0x23, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
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
    if (!parent) {
        printf("fork: get_current_task() == NULL\r\n");
        return -1;
    }

    printf("fork: parent PID=%u  page_dir=0x%x  slot=%d\r\n",
           parent->pid, (uint32_t)parent->page_dir, parent->user_pages_slot);

    /* Parent must have a page directory and user pages slot */
    if (!parent->page_dir) {
        printf("fork: parent has no page directory!\r\n");
        return -1;
    }
    if (parent->user_pages_slot < 0) {
        printf("fork: parent has no user pages slot!\r\n");
        return -1;
    }

    /* Dump the saved user-mode register frame for debugging */
    printf("fork: isr_regs: eip=0x%x  useresp=0x%x  eflags=0x%x\r\n",
           regs->eip, regs->useresp, regs->eflags);
    printf("fork: regs: eax=0x%x ebx=0x%x ecx=0x%x edx=0x%x\r\n",
           regs->eax, regs->ebx, regs->ecx, regs->edx);
    printf("fork: regs: esi=0x%x edi=0x%x ebp=0x%x ds=0x%x\r\n",
           regs->esi, regs->edi, regs->ebp, regs->ds);
    printf("fork: regs: cs=0x%x ss=0x%x\r\n", regs->cs, regs->ss);

    /* Validate user EIP */
    if (regs->eip < 0x08048000u || regs->eip >= 0xC0000000u) {
        printf("fork: INVALID user eip=0x%x\r\n", regs->eip);
        return -1;
    }
    /* Validate user ESP */
    if (regs->useresp < 0x08000000u || regs->useresp >= 0xC0000000u) {
        printf("fork: INVALID user esp=0x%x\r\n", regs->useresp);
        return -1;
    }

    /* Create a new page directory for the child */
    uint32_t *child_pd = paging_create_user_directory();
    if (!child_pd) {
        printf("fork: paging_create_user_directory() failed\r\n");
        return -1;
    }
    printf("fork: child PD at 0x%x\r\n", (uint32_t)child_pd);

    /* Allocate a user pages tracking slot for the child */
    int child_slot = elf_alloc_page_slot();
    if (child_slot < 0) {
        printf("fork: elf_alloc_page_slot() failed (all %d slots busy)\r\n", 16);
        paging_free_user_directory(child_pd);
        return -1;
    }
    printf("fork: child got page slot %d\r\n", child_slot);

    /* Copy all parent user pages to new physical pages in child's PD */
    int parent_slot = parent->user_pages_slot;
    int parent_page_count = elf_get_slot_page_count(parent_slot);
    printf("fork: copying %d user pages from parent slot %d\r\n",
           parent_page_count, parent_slot);

    for (int i = 0; i < parent_page_count; i++) {
        uint32_t vaddr = elf_get_slot_vaddr(parent_slot, i);
        uint32_t parent_paddr = elf_get_slot_paddr(parent_slot, i);

        /* Allocate a new physical page and copy parent's data */
        void *child_page = elf_map_user_page_in(vaddr, child_pd, child_slot);
        if (!child_page) {
            printf("fork: elf_map_user_page_in(0x%x) failed at page %d\r\n", vaddr, i);
            elf_free_page_slot(child_slot);
            paging_free_user_directory(child_pd);
            return -1;
        }

        /* Copy page content from parent */
        memcpy(child_page, (void *)parent_paddr, SYS_PAGE_SIZE);

        /* Verify the mapping is correct in the child PD */
        uint32_t check = paging_get_physical_in(child_pd, vaddr);
        if (check != (uint32_t)child_page) {
            printf("fork: page verify FAILED: vaddr=0x%x  expected=0x%x  got=0x%x\r\n",
                   vaddr, (uint32_t)child_page, check);
        }
    }
    printf("fork: page copy complete\r\n");

    /* Create child task */
    task_t child_task = task_create(fork_child_entry, parent->priority);
    if (child_task.esp == 0 && child_task.stack == NULL) {
        printf("fork: task_create() failed (no kernel stacks?)\r\n");
        elf_free_page_slot(child_slot);
        paging_free_user_directory(child_pd);
        return -1;
    }
    printf("fork: child task PID=%u  kernel_stack=0x%x  esp=0x%x\r\n",
           child_task.pid, (uint32_t)child_task.stack, child_task.esp);

    /* Copy parent state to child */
    child_task.page_dir        = child_pd;
    child_task.user_pages_slot = child_slot;
    child_task.parent_pid      = parent->pid;
    child_task.brk_start       = parent->brk_start;
    child_task.brk_current     = parent->brk_current;

    /* Copy fd table */
    memcpy(child_task.fd_table, parent->fd_table, sizeof(parent->fd_table));

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

    printf("fork: saved child state: eip=0x%x esp=0x%x eflags=0x%x\r\n",
           child_task.fork_eip, child_task.fork_esp, child_task.fork_eflags);

    admit_task(&child_task);
    printf("fork: child admitted, returning child PID %u to parent\r\n",
           child_task.pid);

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
        int found_child = 0;

        /* Scan all tasks for zombie children of this parent */
        /* Try to find a matching zombie child first */
        if (wait_pid > 0) {
            /* Wait for specific PID */
            task_t *child = find_task_by_pid((uint32_t)wait_pid);
            if (!child || child->parent_pid != parent->pid)
                return -1;  /* no such child */
            found_child = 1;
            if (child->state == TASK_ZOMBIE) {
                int32_t child_pid = (int32_t)child->pid;
                if (status)
                    *status = child->exit_code;
                remove_task(child);
                return child_pid;
            }
        } else {
            /* wait_pid == -1: wait for any child */
            /* Scan for any zombie child or any child at all */
            task_t *zombie_child = NULL;
            /* We need to iterate the task list — use find_task_by_pid
             * in a loop or add a dedicated function. For simplicity,
             * just block and retry. */
            /* Check all tasks by iterating PIDs (simple approach) */
            for (uint32_t pid = 0; pid < 1000; pid++) {
                task_t *child = find_task_by_pid(pid);
                if (!child || child->parent_pid != parent->pid)
                    continue;
                found_child = 1;
                if (child->state == TASK_ZOMBIE) {
                    zombie_child = child;
                    break;
                }
            }

            if (zombie_child) {
                int32_t child_pid = (int32_t)zombie_child->pid;
                if (status)
                    *status = zombie_child->exit_code;
                remove_task(zombie_child);
                return child_pid;
            }
        }

        if (!found_child)
            return -1;  /* no children at all → ECHILD */

        /* Child exists but hasn't exited yet — block and retry */
        block_task(parent);
    }
}

typedef int32_t (*syscall_fn_t)(struct isr_regs *regs);

static syscall_fn_t syscall_table[NUM_SYSCALLS] = {
    [SYS_EXIT]     = sys_exit,
    [SYS_WRITE]    = sys_write,
    [SYS_READ]     = sys_read,
    [SYS_GETPID]   = sys_getpid,
    [SYS_BRK]      = sys_brk,
    [SYS_OPEN]     = sys_open,
    [SYS_CLOSE]    = sys_close,
    [SYS_LSEEK]    = sys_lseek,
    [SYS_FBMAP]    = sys_fbmap,
    [SYS_GETTICKS] = sys_getticks,
    [SYS_USLEEP]   = sys_usleep,
    [SYS_WRITE_N]  = sys_write_n,
    [SYS_POLL_KEY] = sys_poll_key,
    [SYS_WAIT_KEY] = sys_wait_key,
    [SYS_GET_WINSIZE] = sys_get_winsize,
    [SYS_STAT]     = sys_stat,
    [SYS_FSTAT]    = sys_fstat,
    [SYS_UNLINK]   = sys_unlink,
    [SYS_GETCWD]   = sys_getcwd,
    [SYS_CHDIR]    = sys_chdir,
    [SYS_GETTIME]  = sys_gettime,
    [SYS_DUP]      = sys_dup,
    [SYS_DUP2]     = sys_dup2,
    [SYS_FORK]     = sys_fork,
    [SYS_WAITPID]  = sys_waitpid,
};

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
        regs->eax = (uint32_t)-1;   /* ENOSYS */
        return;
    }

    int32_t ret = syscall_table[num](regs);
    regs->eax = (uint32_t)ret;
}

/* ------------------------------------------------------------------ */
/*  Initialisation                                                     */
/* ------------------------------------------------------------------ */

void syscall_init(void) {
    isr_register_handler(0x80, syscall_dispatch);
}
