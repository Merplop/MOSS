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

    /* If a parent task (shell) is waiting, unblock it */
    task_t *parent = elf_get_waiting_parent();
    if (parent)
        unblock_task(parent);

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
