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
#define MAX_USER_PAGES  2048
#define MAX_PAGE_SLOTS  16

static uint32_t slot_vaddrs[MAX_PAGE_SLOTS][MAX_USER_PAGES];
static uint32_t slot_paddrs[MAX_PAGE_SLOTS][MAX_USER_PAGES];
static int      slot_count[MAX_PAGE_SLOTS];
static int      slot_in_use[MAX_PAGE_SLOTS];

/* User stack: 16 KiB (4 pages), grows downward from USER_STACK_TOP */
#define USER_STACK_PAGES 4
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

/* Saved between elf_load_and_exec() and user_task_entry(). */
static uint32_t pending_entry_eip;
static uint32_t pending_user_esp;
static task_t  *waiting_parent;  /* shell task to unblock on exit */

/* Called by SYS_EXIT handler to clean up user-mode resources. */
task_t *elf_get_waiting_parent(void) {
    return waiting_parent;
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
    jump_usermode(pending_entry_eip, pending_user_esp);
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
/*  Main loader entry point                                            */
/* ------------------------------------------------------------------ */

int elf_load_and_exec(uint32_t ino, int user_argc, char **user_argv) {
    ext2_inode_t inode;
    if (ext2_read_inode(ino, &inode) != 0) {
        printf("elf: cannot read inode %u\r\n", ino);
        return -1;
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

    printf("elf: loaded, entry=0x%x brk=0x%x\r\n", ehdr.e_entry, highest_vaddr);

    /* ----------------------------------------------------------------
     *  Build argc/argv on the user stack.
     * ---------------------------------------------------------------- */
    uint32_t sp = USER_STACK_TOP;

    uint32_t str_addrs[10];
    int nargs = user_argc;
    if (nargs > 10) nargs = 10;

    for (int i = nargs - 1; i >= 0; i--) {
        size_t len = strlen(user_argv[i]) + 1;
        sp -= len;
        uint32_t page_vaddr = sp & ~(PAGE_SIZE - 1);
        uint32_t phys = paging_get_physical_in(new_pd, page_vaddr);
        if (phys == 0) { elf_free_page_slot(slot); paging_free_user_directory(new_pd); return -1; }
        memcpy((void *)(phys + (sp & (PAGE_SIZE - 1))), user_argv[i], len);
        str_addrs[i] = sp;
    }

    sp &= ~3u;

    sp -= 4;
    {
        uint32_t page_vaddr = sp & ~(PAGE_SIZE - 1);
        uint32_t phys = paging_get_physical_in(new_pd, page_vaddr);
        *(uint32_t *)(phys + (sp & (PAGE_SIZE - 1))) = 0;
    }
    for (int i = nargs - 1; i >= 0; i--) {
        sp -= 4;
        uint32_t page_vaddr = sp & ~(PAGE_SIZE - 1);
        uint32_t phys = paging_get_physical_in(new_pd, page_vaddr);
        *(uint32_t *)(phys + (sp & (PAGE_SIZE - 1))) = str_addrs[i];
    }
    uint32_t argv_start = sp;

    sp -= 4;
    {
        uint32_t page_vaddr = sp & ~(PAGE_SIZE - 1);
        uint32_t phys = paging_get_physical_in(new_pd, page_vaddr);
        *(uint32_t *)(phys + (sp & (PAGE_SIZE - 1))) = argv_start;
    }
    sp -= 4;
    {
        uint32_t page_vaddr = sp & ~(PAGE_SIZE - 1);
        uint32_t phys = paging_get_physical_in(new_pd, page_vaddr);
        *(uint32_t *)(phys + (sp & (PAGE_SIZE - 1))) = (uint32_t)nargs;
    }

    /* Save entry point for the new task */
    pending_entry_eip = ehdr.e_entry;
    pending_user_esp  = sp;
    waiting_parent    = get_current_task();

    /* Create a new scheduler task that will jump to user mode */
    task_t user_task = task_create(user_task_entry, DEFAULT_PRIORITY);
    user_task.brk_start   = highest_vaddr;
    user_task.brk_current = highest_vaddr;
    user_task.page_dir    = new_pd;
    user_task.user_pages_slot = slot;
    user_task.parent_pid  = waiting_parent ? waiting_parent->pid : 0;
    admit_task(&user_task);

    /* Block the calling task (shell) until the user program exits.
     * SYS_EXIT in syscall.c will unblock us. */
    block_task(get_current_task());

    /* When we resume here, the user program has exited.
     * Cleanup is done by SYS_EXIT → elf_cleanup_process(). */

    return 0;
}
