/*
 * ELF32 loader for i386.
 * MOSS Kernel
 *
 * Loads a statically-linked ELF executable from the ext2 filesystem,
 * maps its PT_LOAD segments into the address space with user-accessible
 * page flags, sets up a user-mode stack, and enters ring 3 via iret.
 *
 * A new scheduler task is created for the user program.  The calling
 * task (typically the shell) blocks until the program exits via SYS_EXIT.
 *
 * Physical pages for user segments and stack are obtained from the
 * kernel's physical memory allocator (allocate_blocks).
 *
 * Each user process gets its own page directory (per-process address
 * space) and a user page tracking slot for cleanup on exit.
 */

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#include <kernel/elf.h>
#include <kernel/ext2.h>
#include <kernel/kernel.h>
#include <kernel/sched.h>
#include <kernel/memory_manager.h>

/* Arch-specific paging API */
void paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags);
void paging_map_page_in(uint32_t *pd, uint32_t virt, uint32_t phys, uint32_t flags);
void paging_unmap_page(uint32_t virt);
uint32_t paging_get_physical(uint32_t virt);
uint32_t paging_get_physical_in(uint32_t *pd, uint32_t virt);
uint32_t *paging_get_page_dir(void);
uint32_t *paging_create_user_directory(void);
void paging_free_user_directory(uint32_t *pd);
void paging_switch_directory(uint32_t *pd);

/* Paging flags */
#define PTE_PRESENT  0x001
#define PTE_WRITABLE 0x002
#define PTE_USER     0x004

/* GDT selectors (from tss.h) */
#define GDT_USER_CODE  0x1B   /* 0x18 | RPL 3 */
#define GDT_USER_DATA  0x23   /* 0x20 | RPL 3 */

/* PDE flags needed so page tables are user-accessible */
#define PDE_PRESENT  0x001
#define PDE_WRITABLE 0x002
#define PDE_USER     0x004

/* ------------------------------------------------------------------ */
/*  Per-process user page tracking                                     */
/* ------------------------------------------------------------------ */

#define PAGE_SIZE       4096
#define MAX_USER_PAGES  4096
#define MAX_PAGE_SLOTS  16

static uint32_t slot_vaddrs[MAX_PAGE_SLOTS][MAX_USER_PAGES];
static uint32_t slot_paddrs[MAX_PAGE_SLOTS][MAX_USER_PAGES];
static int      slot_count[MAX_PAGE_SLOTS];
static int      slot_in_use[MAX_PAGE_SLOTS];

/* User stack: 1 MiB (256 pages), grows downward from USER_STACK_TOP */
#define USER_STACK_PAGES 256
#define USER_STACK_TOP   0xBFFFF000u
#define USER_STACK_BASE  (USER_STACK_TOP - USER_STACK_PAGES * PAGE_SIZE)

/* Allocate a user pages tracking slot. Returns slot index or -1. */
int elf_alloc_page_slot(void) {
    for (int i = 0; i < MAX_PAGE_SLOTS; i++) {
        if (!slot_in_use[i]) {
            slot_in_use[i] = 1;
            slot_count[i] = 0;
            return i;
        }
    }
    return -1;
}

/* Map a user page into a specific page directory and track it in a slot. */
void *elf_map_user_page_in(uint32_t vaddr, uint32_t *page_dir, int slot) {
    if (slot < 0 || slot >= MAX_PAGE_SLOTS)
        return NULL;
    if (slot_count[slot] >= MAX_USER_PAGES)
        return NULL;

    uint32_t *phys = allocate_blocks(1);
    if (!phys)
        return NULL;

    uint32_t phys_addr = (uint32_t)phys;

    /* Zero the page (kernel has identity mapping, so phys == virt for kernel) */
    memset(phys, 0, PAGE_SIZE);

    paging_map_page_in(page_dir, vaddr & ~(PAGE_SIZE - 1), phys_addr,
                       PTE_PRESENT | PTE_WRITABLE | PTE_USER);

    /* Ensure the PDE is also user-accessible */
    page_dir[vaddr >> 22] |= PDE_USER;

    slot_vaddrs[slot][slot_count[slot]] = vaddr & ~(PAGE_SIZE - 1);
    slot_paddrs[slot][slot_count[slot]] = phys_addr;
    slot_count[slot]++;

    return phys;
}

/* Map a user page using the current task's page directory and slot. */
void *elf_map_user_page(uint32_t vaddr) {
    task_t *t = get_current_task();
    if (!t || t->user_pages_slot < 0 || !t->page_dir)
        return NULL;
    return elf_map_user_page_in(vaddr, t->page_dir, t->user_pages_slot);
}

/* Free all user pages in a slot and release the slot. */
void elf_free_page_slot(int slot) {
    if (slot < 0 || slot >= MAX_PAGE_SLOTS || !slot_in_use[slot])
        return;
    for (int i = 0; i < slot_count[slot]; i++) {
        free_blocks((uint32_t *)slot_paddrs[slot][i], 1);
    }
    slot_count[slot] = 0;
    slot_in_use[slot] = 0;
}

/* Clean up a process's user pages, page tables, and page directory. */
void elf_cleanup_process(task_t *task) {
    if (!task)
        return;
    if (task->user_pages_slot >= 0)
        elf_free_page_slot(task->user_pages_slot);
    if (task->page_dir && task->page_dir != paging_get_page_dir())
        paging_free_user_directory(task->page_dir);
    task->user_pages_slot = -1;
    task->page_dir = NULL;
}

/* Legacy compatibility wrapper */
void elf_reset_user_pages(void) {
    /* No-op: cleanup is now per-process via elf_cleanup_process */
}

/* Get the number of user pages in a slot. */
int elf_get_slot_page_count(int slot) {
    if (slot < 0 || slot >= MAX_PAGE_SLOTS)
        return 0;
    return slot_count[slot];
}

/* Get user page virtual/physical addresses from a slot. */
uint32_t elf_get_slot_vaddr(int slot, int index) {
    return slot_vaddrs[slot][index];
}

uint32_t elf_get_slot_paddr(int slot, int index) {
    return slot_paddrs[slot][index];
}

/* ------------------------------------------------------------------ */
/*  User-program task state                                            */
/* ------------------------------------------------------------------ */

static task_t  *waiting_parent;      /* shell task to unblock on exit */
static uint32_t waiting_child_pid;   /* PID of the child that should trigger unblock */

/* Called by SYS_EXIT handler to clean up user-mode resources. */
task_t *elf_get_waiting_parent(void) {
    return waiting_parent;
}

uint32_t elf_get_waiting_child_pid(void) {
    return waiting_child_pid;
}

void elf_clear_waiting_parent(void) {
    waiting_parent = NULL;
    waiting_child_pid = 0;
}

/* ------------------------------------------------------------------ */
/*  Assembly: enter ring 3 via iret                                    */
/* ------------------------------------------------------------------ */

static void __attribute__((noreturn))
jump_usermode(uint32_t entry_eip, uint32_t user_esp) {
    asm volatile(
        "cli\n\t"
        "movw $0x23, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "pushl $0x23\n\t"        /* SS  = GDT_USER_DATA */
        "pushl %1\n\t"           /* ESP = user_esp */
        "pushfl\n\t"
        "popl %%eax\n\t"
        "orl $0x200, %%eax\n\t"  /* Set IF */
        "pushl %%eax\n\t"
        "pushl $0x1B\n\t"        /* CS  = GDT_USER_CODE */
        "pushl %0\n\t"           /* EIP = entry_eip */
        "iret\n\t"
        :
        : "r"(entry_eip), "r"(user_esp)
        : "eax", "memory"
    );
    __builtin_unreachable();
}

/* Entry function for the user-program scheduler task.
 * This runs on the new task's kernel stack, then does iret to ring 3. */
static void user_task_entry(void) {
    task_t *self = get_current_task();
    jump_usermode(self->user_entry_eip, self->user_entry_esp);
}

/* ------------------------------------------------------------------ */
/*  ELF validation                                                     */
/* ------------------------------------------------------------------ */

static int elf_validate(const Elf32_Ehdr *ehdr) {
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        printf("elf: bad magic\r\n");
        return -1;
    }
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
        printf("elf: not ELF32\r\n");
        return -1;
    }
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        printf("elf: not little-endian\r\n");
        return -1;
    }
    if (ehdr->e_type != ET_EXEC) {
        printf("elf: not an executable (type=%d)\r\n", ehdr->e_type);
        return -1;
    }
    if (ehdr->e_machine != EM_386) {
        printf("elf: not i386 (machine=%d)\r\n", ehdr->e_machine);
        return -1;
    }
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        printf("elf: no program headers\r\n");
        return -1;
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Shebang (#!) script handling                                       */
/* ------------------------------------------------------------------ */

/**
 * Check if a file starts with "#!" and if so, parse the interpreter
 * path and optional argument.  Returns 0 if it's a shebang script
 * (interp_path and interp_arg filled in), -1 if not a script.
 */
static int parse_shebang(uint32_t ino, char *interp_path, size_t path_size,
                         char *interp_arg, size_t arg_size) {
    char buf[256];
    int n = ext2_read_file(ino, buf, 0, sizeof(buf) - 1);
    if (n < 2)
        return -1;
    buf[n] = '\0';

    if (buf[0] != '#' || buf[1] != '!')
        return -1;

    /* Find end of line */
    char *line = buf + 2;
    char *eol = line;
    while (*eol && *eol != '\n' && *eol != '\r')
        eol++;
    *eol = '\0';

    /* Skip leading whitespace */
    while (*line == ' ' || *line == '\t')
        line++;

    if (*line == '\0')
        return -1;

    /* Extract interpreter path */
    char *end = line;
    while (*end && *end != ' ' && *end != '\t')
        end++;

    size_t plen = (size_t)(end - line);
    if (plen >= path_size)
        plen = path_size - 1;
    memcpy(interp_path, line, plen);
    interp_path[plen] = '\0';

    /* Extract optional argument */
    interp_arg[0] = '\0';
    if (*end) {
        char *arg = end;
        while (*arg == ' ' || *arg == '\t')
            arg++;
        if (*arg) {
            size_t alen = strlen(arg);
            if (alen >= arg_size)
                alen = arg_size - 1;
            memcpy(interp_arg, arg, alen);
            interp_arg[alen] = '\0';
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Main loader entry point                                            */
/* ------------------------------------------------------------------ */

int elf_load_and_exec(uint32_t ino, int user_argc, char **user_argv) {
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) {
        printf("elf: cannot read inode %u\r\n", ino);
        return -1;
    }
    if (inode.i_size < 4) {
        printf("elf: file too small\r\n");
        return -1;
    }

    /* Check for shebang script (#!) — resolve iteratively */
    char *exec_argv[66];
    int exec_argc = user_argc;
    char **exec_argv_ptr = user_argv;
    char interp_path[256];
    char interp_arg[256];
    if (parse_shebang(ino, interp_path, sizeof(interp_path),
                      interp_arg, sizeof(interp_arg)) == 0) {
        /* It's a script — resolve the interpreter */
        uint32_t interp_ino = fs_resolve_path(interp_path);
        if (interp_ino == 0) {
            printf("elf: interpreter '%s' not found\r\n", interp_path);
            return -1;
        }
        /* Build new argv: [interpreter, interp_arg?, script_path, original_args...] */
        int new_argc = 0;
        exec_argv[new_argc++] = interp_path;
        if (interp_arg[0])
            exec_argv[new_argc++] = interp_arg;
        /* Add the script path (argv[0] from original invocation) */
        if (user_argc > 0)
            exec_argv[new_argc++] = user_argv[0];
        /* Add remaining original arguments */
        for (int i = 1; i < user_argc && new_argc < 65; i++)
            exec_argv[new_argc++] = user_argv[i];
        exec_argv[new_argc] = NULL;
        /* Switch to the interpreter */
        ino = interp_ino;
        exec_argc = new_argc;
        exec_argv_ptr = exec_argv;
        /* Re-read the interpreter inode */
        if (ext2_read_inode(ino, &inode) != 0) {
            printf("elf: cannot read interpreter inode\r\n");
            return -1;
        }
    }

    if (inode.i_size < sizeof(Elf32_Ehdr)) {
        printf("elf: file too small\r\n");
        return -1;
    }

    /* Read the ELF header */
    Elf32_Ehdr ehdr;
    if (ext2_read_file(ino, &ehdr, 0, sizeof(ehdr)) != (int)sizeof(ehdr)) {
        printf("elf: failed to read ELF header\r\n");
        return -1;
    }
    if (elf_validate(&ehdr) != 0)
        return -1;

    /* Read all program headers */
    uint32_t ph_table_size = ehdr.e_phnum * ehdr.e_phentsize;
    if (ph_table_size > 1024) {
        printf("elf: program header table too large\r\n");
        return -1;
    }
    uint8_t phdr_buf[1024];
    if (ext2_read_file(ino, phdr_buf, ehdr.e_phoff, ph_table_size)
        != (int)ph_table_size) {
        printf("elf: failed to read program headers\r\n");
        return -1;
    }

    /* Create per-process page directory and user pages slot */
    uint32_t *new_pd = paging_create_user_directory();
    if (!new_pd) {
        printf("elf: cannot allocate page directory\r\n");
        return -1;
    }

    int slot = elf_alloc_page_slot();
    if (slot < 0) {
        printf("elf: no free page slots\r\n");
        paging_free_user_directory(new_pd);
        return -1;
    }

    /* Track the highest virtual address across all PT_LOAD segments
     * so we can set up the program break for brk/sbrk. */
    uint32_t highest_vaddr = 0;

    /* Process each PT_LOAD segment */
    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr *ph = (Elf32_Phdr *)(phdr_buf + i * ehdr.e_phentsize);

        if (ph->p_type != PT_LOAD)
            continue;
        if (ph->p_memsz == 0)
            continue;

        uint32_t seg_start = ph->p_vaddr & ~(PAGE_SIZE - 1);
        uint32_t seg_end   = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1)
                             & ~(PAGE_SIZE - 1);

        if (seg_end > highest_vaddr)
            highest_vaddr = seg_end;

        for (uint32_t addr = seg_start; addr < seg_end; addr += PAGE_SIZE) {
            if (elf_map_user_page_in(addr, new_pd, slot) == NULL) {
                printf("elf: out of user pages at vaddr 0x%x\r\n", addr);
                elf_free_page_slot(slot);
                paging_free_user_directory(new_pd);
                return -1;
            }
        }

        /* Copy file data into the mapped pages */
        uint32_t bytes_left = ph->p_filesz;
        uint32_t file_off = ph->p_offset;
        uint32_t vaddr = ph->p_vaddr;

        while (bytes_left > 0) {
            uint32_t page_vaddr = vaddr & ~(PAGE_SIZE - 1);
            uint32_t page_off   = vaddr & (PAGE_SIZE - 1);
            uint32_t chunk = PAGE_SIZE - page_off;
            if (chunk > bytes_left)
                chunk = bytes_left;

            uint32_t phys = paging_get_physical_in(new_pd, page_vaddr);
            if (phys == 0) {
                printf("elf: unmapped page at 0x%x\r\n", page_vaddr);
                elf_free_page_slot(slot);
                paging_free_user_directory(new_pd);
                return -1;
            }
            uint8_t *dst = (uint8_t *)(phys + page_off);

            if (ext2_read_file(ino, dst, file_off, chunk) != (int)chunk) {
                printf("elf: failed to read segment data\r\n");
                elf_free_page_slot(slot);
                paging_free_user_directory(new_pd);
                return -1;
            }

            file_off   += chunk;
            vaddr      += chunk;
            bytes_left -= chunk;
        }
    }

    /* Set up user-mode stack */
    for (uint32_t addr = USER_STACK_BASE; addr < USER_STACK_TOP;
         addr += PAGE_SIZE) {
        if (elf_map_user_page_in(addr, new_pd, slot) == NULL) {
            printf("elf: cannot allocate user stack\r\n");
            elf_free_page_slot(slot);
            paging_free_user_directory(new_pd);
            return -1;
        }
    }

    //printf("elf: loaded, entry=0x%x brk=0x%x\r\n", ehdr.e_entry, highest_vaddr);

    /* ----------------------------------------------------------------
     *  Build argc/argv on the user stack.
     * ---------------------------------------------------------------- */
    uint32_t sp = USER_STACK_TOP;

    uint32_t str_addrs[10];
    int nargs = exec_argc;
    if (nargs > 10) nargs = 10;

    for (int i = nargs - 1; i >= 0; i--) {
        size_t len = strlen(exec_argv_ptr[i]) + 1;
        sp -= len;
        uint32_t page_vaddr = sp & ~(PAGE_SIZE - 1);
        uint32_t phys = paging_get_physical_in(new_pd, page_vaddr);
        if (phys == 0) { elf_free_page_slot(slot); paging_free_user_directory(new_pd); return -1; }
        memcpy((void *)(phys + (sp & (PAGE_SIZE - 1))), exec_argv_ptr[i], len);
        str_addrs[i] = sp;
    }

    /* envp: push default environment variables */
    static const char *default_env[] = {
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
    int nenv = 0;
    for (int i = 0; default_env[i]; i++) nenv++;
    uint32_t env_addrs[16];

    /* Push env strings onto user stack */
    for (int i = nenv - 1; i >= 0; i--) {
        size_t len = strlen(default_env[i]) + 1;
        sp -= len;
        uint32_t page_vaddr = sp & ~(PAGE_SIZE - 1);
        uint32_t phys = paging_get_physical_in(new_pd, page_vaddr);
        if (phys == 0) { elf_free_page_slot(slot); paging_free_user_directory(new_pd); return -1; }
        memcpy((void *)(phys + (sp & (PAGE_SIZE - 1))), default_env[i], len);
        env_addrs[i] = sp;
    }

    sp &= ~0xFu;  /* 16-byte align before pointer table */

    /* Helper macro to push a 32-bit value onto the user stack */
    #define PUSH32(val) do { \
        sp -= 4; \
        uint32_t _pv = sp & ~(PAGE_SIZE - 1); \
        uint32_t _ph = paging_get_physical_in(new_pd, _pv); \
        if (_ph == 0) { elf_free_page_slot(slot); paging_free_user_directory(new_pd); return -1; } \
        *(uint32_t *)(_ph + (sp & (PAGE_SIZE - 1))) = (uint32_t)(val); \
    } while (0)

    /* Auxiliary vector (auxv) — pushed first so it sits at highest
     * addresses, right after the envp NULL in memory layout:
     * [argc, argv..., NULL, envp..., NULL, auxv..., AT_NULL] */
    PUSH32(0); PUSH32(0);     /* AT_NULL(0) = end of auxv */
    PUSH32(4096); PUSH32(6);  /* AT_PAGESZ(6) = 4096 */

    /* envp NULL terminator */
    PUSH32(0);

    /* envp pointers */
    for (int i = nenv - 1; i >= 0; i--)
        PUSH32(env_addrs[i]);

    /* argv NULL terminator */
    PUSH32(0);

    /* argv pointers (reversed so argv[0] is at lowest address) */
    for (int i = nargs - 1; i >= 0; i--)
        PUSH32(str_addrs[i]);

    /* argc */
    PUSH32(nargs);

    #undef PUSH32

    waiting_parent = get_current_task();

    /* Create a new scheduler task that will jump to user mode */
    task_t user_task = task_create(user_task_entry, DEFAULT_PRIORITY);
    user_task.user_entry_eip = ehdr.e_entry;
    user_task.user_entry_esp = sp;
    user_task.brk_start   = highest_vaddr;
    user_task.brk_current = highest_vaddr;
    user_task.page_dir    = new_pd;
    user_task.user_pages_slot = slot;
    user_task.parent_pid  = waiting_parent ? waiting_parent->pid : 0;
    admit_task(&user_task);

    /* Record which child PID should trigger the unblock */
    waiting_child_pid = user_task.pid;

    /* Block the calling task (shell) until the user program exits.
     * SYS_EXIT in syscall.c will unblock us. */
    block_task(get_current_task());

    /* When we resume here, the user program has exited.
     * Cleanup is done by SYS_EXIT → elf_cleanup_process(). */

    return 0;
}

/* ------------------------------------------------------------------ */
/*  execve: replace current process image with a new ELF               */
/* ------------------------------------------------------------------ */

/*
 * elf_exec_replace() — loads a new ELF into the CURRENT task's address
 * space, destroying the old image.  On success, never returns (jumps
 * to user mode).  On failure, returns -1 (caller must handle).
 *
 * Parameters:
 *   ino       — ext2 inode number of the ELF executable
 *   argc      — argument count
 *   argv      — argument vector (kernel-side copies)
 *   envc      — environment variable count
 *   envp      — environment vector (kernel-side copies)
 *
 * This is called from the SYS_EXECVE handler.
 */
int elf_exec_replace(uint32_t ino, int argc, char **argv, int envc, char **envp) {
    task_t *current = get_current_task();
    if (!current)
        return -1;

    /* Resolve shebang scripts iteratively (avoids recursive call that
     * would double the ~6KB stack frame and overflow the 16KB kernel stack). */
    char shebang_buf[4096];
    char *shebang_argv[66];
    int shebang_argc = 0;

    {
        char interp_path[256];
        char interp_arg[256];
        if (parse_shebang(ino, interp_path, sizeof(interp_path),
                          interp_arg, sizeof(interp_arg)) == 0) {
            /* It's a script — resolve the interpreter */
            uint32_t interp_ino = fs_resolve_path(interp_path);
            if (interp_ino == 0)
                return -1;
            /* Build new argv: [interpreter, interp_arg?, script_path, original_args...] */
            int off = 0;
            /* Copy interpreter path */
            size_t plen = strlen(interp_path) + 1;
            memcpy(shebang_buf + off, interp_path, plen);
            shebang_argv[shebang_argc++] = shebang_buf + off;
            off += (int)plen;
            /* Copy optional interpreter argument */
            if (interp_arg[0]) {
                size_t alen = strlen(interp_arg) + 1;
                memcpy(shebang_buf + off, interp_arg, alen);
                shebang_argv[shebang_argc++] = shebang_buf + off;
                off += (int)alen;
            }
            /* Copy original argv (script path + args) */
            for (int i = 0; i < argc && shebang_argc < 65; i++) {
                size_t l = strlen(argv[i]) + 1;
                if (off + (int)l > (int)sizeof(shebang_buf))
                    break;
                memcpy(shebang_buf + off, argv[i], l);
                shebang_argv[shebang_argc++] = shebang_buf + off;
                off += (int)l;
            }
            shebang_argv[shebang_argc] = NULL;
            /* Switch to the interpreter */
            ino = interp_ino;
            argc = shebang_argc;
            argv = shebang_argv;
        }
    }

    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0)
        return -1;

    if (inode.i_size < sizeof(Elf32_Ehdr))
        return -1;

    /* Read and validate ELF header */
    Elf32_Ehdr ehdr;
    if (ext2_read_file(ino, &ehdr, 0, sizeof(ehdr)) != (int)sizeof(ehdr))
        return -1;
    if (elf_validate(&ehdr) != 0)
        return -1;

    /* Read program headers */
    uint32_t ph_table_size = ehdr.e_phnum * ehdr.e_phentsize;
    if (ph_table_size > 1024)
        return -1;
    uint8_t phdr_buf[1024];
    if (ext2_read_file(ino, phdr_buf, ehdr.e_phoff, ph_table_size)
        != (int)ph_table_size)
        return -1;

    /* --- Point of no return: destroy old address space --- */

    /* Disable interrupts to prevent the scheduler from seeing a
     * half-torn-down address space (dangling page_dir pointer). */
    asm volatile("cli");

    /* Save old PD/slot BEFORE allocating new ones — we must not free
     * the old PD while CR3 still points to it, because:
     *   1. The allocator could reuse the frame, and
     *   2. paging_switch_directory() skips the CR3 write (no TLB flush)
     *      when the new PD happens to be at the same physical address.
     * Allocate the new PD first, switch CR3, THEN free the old one. */
    uint32_t *old_pd   = current->page_dir;
    int       old_slot = current->user_pages_slot;

    uint32_t *new_pd = paging_create_user_directory();
    if (!new_pd) {
        asm volatile("sti");
        exit_task(-1);
        __builtin_unreachable();
    }

    int slot = elf_alloc_page_slot();
    if (slot < 0) {
        paging_free_user_directory(new_pd);
        asm volatile("sti");
        exit_task(-1);
        __builtin_unreachable();
    }

    /* Update task with new page directory */
    current->page_dir = new_pd;
    current->user_pages_slot = slot;

    /* Switch CR3 to the new page directory (guaranteed different address
     * from old_pd since old_pd hasn't been freed yet → TLB is flushed). */
    paging_switch_directory(new_pd);

    /* NOW it's safe to tear down the old address space */
    if (old_slot >= 0)
        elf_free_page_slot(old_slot);
    if (old_pd)
        paging_free_user_directory(old_pd);

    /* Clear stale TLS state inherited from fork parent — the new
     * program will call set_thread_area to set up its own TLS. */
    current->tls_gs    = 0;
    current->tls_entry = 0;
    current->tls_base  = 0;
    current->tls_limit = 0;

    /* POSIX: exec resets caught signals to SIG_DFL (ignored signals stay) */
    for (int i = 1; i < _NSIG; i++) {
        if (current->sig_handlers[i] != SIG_IGN)
            current->sig_handlers[i] = SIG_DFL;
    }
    current->sig_pending = 0;
    current->in_signal   = 0;

    asm volatile("sti");

    /* Track highest vaddr for brk */
    uint32_t highest_vaddr = 0;

    /* Load PT_LOAD segments */
    for (int i = 0; i < ehdr.e_phnum; i++) {
        Elf32_Phdr *ph = (Elf32_Phdr *)(phdr_buf + i * ehdr.e_phentsize);
        if (ph->p_type != PT_LOAD || ph->p_memsz == 0)
            continue;

        uint32_t seg_start = ph->p_vaddr & ~(PAGE_SIZE - 1);
        uint32_t seg_end   = (ph->p_vaddr + ph->p_memsz + PAGE_SIZE - 1)
                             & ~(PAGE_SIZE - 1);

        if (seg_end > highest_vaddr)
            highest_vaddr = seg_end;

        for (uint32_t addr = seg_start; addr < seg_end; addr += PAGE_SIZE) {
            if (elf_map_user_page_in(addr, new_pd, slot) == NULL) {
                exit_task(-1);
                __builtin_unreachable();
            }
        }

        /* Copy file data */
        uint32_t bytes_left = ph->p_filesz;
        uint32_t file_off = ph->p_offset;
        uint32_t vaddr = ph->p_vaddr;

        while (bytes_left > 0) {
            uint32_t page_vaddr = vaddr & ~(PAGE_SIZE - 1);
            uint32_t page_off   = vaddr & (PAGE_SIZE - 1);
            uint32_t chunk = PAGE_SIZE - page_off;
            if (chunk > bytes_left)
                chunk = bytes_left;

            uint32_t phys = paging_get_physical_in(new_pd, page_vaddr);
            if (phys == 0) {
                exit_task(-1);
                __builtin_unreachable();
            }
            uint8_t *dst = (uint8_t *)(phys + page_off);

            if (ext2_read_file(ino, dst, file_off, chunk) != (int)chunk) {
                exit_task(-1);
                __builtin_unreachable();
            }

            file_off   += chunk;
            vaddr      += chunk;
            bytes_left -= chunk;
        }
    }

    /* Allocate user stack */
    for (uint32_t addr = USER_STACK_BASE; addr < USER_STACK_TOP;
         addr += PAGE_SIZE) {
        if (elf_map_user_page_in(addr, new_pd, slot) == NULL) {
            exit_task(-1);
            __builtin_unreachable();
        }
    }

    /* Set up program break */
    current->brk_start   = highest_vaddr;
    current->brk_current = highest_vaddr;

    /* Build argc/argv/envp on user stack (Linux-style ABI layout).
     *
     * String area (high addresses, pushed first):
     *   envp strings, argv strings
     *
     * Then the pointer/integer area (low addresses):
     *   sp+0:  argc
     *   sp+4:  argv[0] ptr
     *          ...
     *          NULL
     *          envp[0] ptr
     *          ...
     *          NULL
     */
    uint32_t sp = USER_STACK_TOP;

    int nargs = argc;
    if (nargs > 64) nargs = 64;
    int nenv = envc;
    if (nenv > 64) nenv = 64;

    uint32_t str_addrs[64];
    uint32_t env_addrs[64];

    /* Push environment strings (high to low) */
    for (int i = nenv - 1; i >= 0; i--) {
        size_t len = strlen(envp[i]) + 1;
        sp -= len;
        uint32_t page_vaddr = sp & ~(PAGE_SIZE - 1);
        uint32_t phys = paging_get_physical_in(new_pd, page_vaddr);
        if (phys == 0) { exit_task(-1); __builtin_unreachable(); }
        memcpy((void *)(phys + (sp & (PAGE_SIZE - 1))), envp[i], len);
        env_addrs[i] = sp;
    }

    /* Push argument strings */
    for (int i = nargs - 1; i >= 0; i--) {
        size_t len = strlen(argv[i]) + 1;
        sp -= len;
        uint32_t page_vaddr = sp & ~(PAGE_SIZE - 1);
        uint32_t phys = paging_get_physical_in(new_pd, page_vaddr);
        if (phys == 0) { exit_task(-1); __builtin_unreachable(); }
        memcpy((void *)(phys + (sp & (PAGE_SIZE - 1))), argv[i], len);
        str_addrs[i] = sp;
    }

    sp &= ~0xFu;  /* 16-byte align before pointer table */

    /* Helper macro to push a 32-bit value onto the user stack */
    #define PUSH32(val) do { \
        sp -= 4; \
        uint32_t _pv = sp & ~(PAGE_SIZE - 1); \
        uint32_t _ph = paging_get_physical_in(new_pd, _pv); \
        if (_ph == 0) { exit_task(-1); __builtin_unreachable(); } \
        *(uint32_t *)(_ph + (sp & (PAGE_SIZE - 1))) = (uint32_t)(val); \
    } while (0)

    /* Auxiliary vector (auxv) — pushed first so it sits at highest
     * addresses, right after the envp NULL in memory layout:
     * [argc, argv..., NULL, envp..., NULL, auxv..., AT_NULL] */
    PUSH32(0); PUSH32(0);     /* AT_NULL(0) = end of auxv */
    PUSH32(4096); PUSH32(6);  /* AT_PAGESZ(6) = 4096 */

    /* Push envp NULL terminator */
    PUSH32(0);

    /* Push envp pointers */
    for (int i = nenv - 1; i >= 0; i--)
        PUSH32(env_addrs[i]);

    /* Push argv NULL terminator */
    PUSH32(0);

    /* Push argv pointers */
    for (int i = nargs - 1; i >= 0; i--)
        PUSH32(str_addrs[i]);

    /* Push argc */
    PUSH32(nargs);

    #undef PUSH32

    /* Close all fd's marked close-on-exec (for now, none — but reset
     * file offsets isn't needed for execve semantics; fds are inherited) */

    /* Jump to user mode — this never returns */
    jump_usermode(ehdr.e_entry, sp);
    __builtin_unreachable();
}
