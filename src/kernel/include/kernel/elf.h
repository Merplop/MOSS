#ifndef _KERNEL_ELF_H
#define _KERNEL_ELF_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  ELF32 data types and structures                                    */
/* ------------------------------------------------------------------ */

typedef uint32_t Elf32_Addr;
typedef uint16_t Elf32_Half;
typedef uint32_t Elf32_Off;
typedef int32_t  Elf32_Sword;
typedef uint32_t Elf32_Word;

/* e_ident indices */
#define EI_MAG0    0
#define EI_MAG1    1
#define EI_MAG2    2
#define EI_MAG3    3
#define EI_CLASS   4
#define EI_DATA    5
#define EI_VERSION 6
#define EI_NIDENT  16

/* ELF magic */
#define ELFMAG0 0x7F
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

/* e_ident[EI_CLASS] */
#define ELFCLASS32 1

/* e_ident[EI_DATA] */
#define ELFDATA2LSB 1   /* little-endian */

/* e_type */
#define ET_EXEC 2       /* executable file */

/* e_machine */
#define EM_386  3       /* Intel 80386 */

/* ELF header */
typedef struct {
    uint8_t    e_ident[EI_NIDENT];
    Elf32_Half e_type;
    Elf32_Half e_machine;
    Elf32_Word e_version;
    Elf32_Addr e_entry;        /* entry point virtual address */
    Elf32_Off  e_phoff;        /* program header table offset */
    Elf32_Off  e_shoff;        /* section header table offset */
    Elf32_Word e_flags;
    Elf32_Half e_ehsize;
    Elf32_Half e_phentsize;
    Elf32_Half e_phnum;        /* number of program headers */
    Elf32_Half e_shentsize;
    Elf32_Half e_shnum;
    Elf32_Half e_shstrndx;
} __attribute__((packed)) Elf32_Ehdr;

/* Program header */
typedef struct {
    Elf32_Word p_type;
    Elf32_Off  p_offset;       /* offset in file */
    Elf32_Addr p_vaddr;        /* virtual address in memory */
    Elf32_Addr p_paddr;        /* physical address (unused) */
    Elf32_Word p_filesz;       /* size in file */
    Elf32_Word p_memsz;        /* size in memory (may be > filesz for .bss) */
    Elf32_Word p_flags;
    Elf32_Word p_align;
} __attribute__((packed)) Elf32_Phdr;

/* p_type values */
#define PT_NULL    0
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3
#define PT_NOTE    4

/* p_flags */
#define PF_X 0x1   /* execute */
#define PF_W 0x2   /* write */
#define PF_R 0x4   /* read */

/* ------------------------------------------------------------------ */
/*  ELF loader API                                                     */
/* ------------------------------------------------------------------ */

/* Load an ELF executable from ext2 inode `ino`, map its segments into
 * the current address space with user-accessible pages, and execute it
 * in ring 3 on a new scheduler task.  The calling task blocks until
 * the user program exits via SYS_EXIT.
 * `user_argc` and `user_argv` are passed to the program's main().
 * Returns 0 on success (after the program exits), -1 on error. */
int elf_load_and_exec(uint32_t ino, int user_argc, char **user_argv);

/* Clean up user-mode page mappings (legacy no-op). */
void elf_reset_user_pages(void);

/* Get the task that should be unblocked when the user program exits. */
struct task_s;
struct task_s *elf_get_waiting_parent(void);

/* Map a zeroed physical page at a user-space virtual address.
 * Uses the current task's page directory and page slot.
 * Returns a pointer to the page (identity-mapped), or NULL on failure. */
void *elf_map_user_page(uint32_t vaddr);

/* Map a zeroed physical page into a specific page directory and slot. */
void *elf_map_user_page_in(uint32_t vaddr, uint32_t *page_dir, int slot);

/* Allocate a user pages tracking slot. Returns slot index or -1. */
int elf_alloc_page_slot(void);

/* Free all user pages in a slot and release it. */
void elf_free_page_slot(int slot);

/* Clean up a process's user pages, page tables, and page directory. */
void elf_cleanup_process(struct task_s *task);

/* Get slot page info (for fork duplication). */
int elf_get_slot_page_count(int slot);
uint32_t elf_get_slot_vaddr(int slot, int index);
uint32_t elf_get_slot_paddr(int slot, int index);

#endif /* _KERNEL_ELF_H */
