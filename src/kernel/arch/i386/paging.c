/*
 * Simple virtual-memory / paging implementation for i386.
 *
 * Sets up two-level paging (page directory + page tables) with an
 * identity map so that virtual == physical for all available RAM.
 * Provides helpers to map/unmap individual 4 KiB pages and a page-fault
 * handler that prints diagnostic information.
 *
 * Page tables are statically allocated in BSS to avoid depending on the
 * physical memory manager (which may not be fully functional this early).
 *
 * MOSS Kernel – i386
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <kernel/kernel.h>
#include <kernel/memory_manager.h>
#include "idt.h"
#include "paging.h"

/*
 * Maximum number of page tables we keep in BSS.
 * 1024 tables × 4 MiB each = full 4 GiB 32-bit address space.
 * BSS cost: 1024 × 4 KiB = 4 MiB (no disk overhead, acceptable for a kernel).
 */
#define MAX_STATIC_TABLES 1024

/* ------------------------------------------------------------------ */
/*  Static storage – all page-aligned, lives in BSS                   */
/* ------------------------------------------------------------------ */
static uint32_t kernel_page_dir[TABLES_PER_DIR]
    __attribute__((aligned(PAGE_SIZE)));

static uint32_t page_table_pool[MAX_STATIC_TABLES][PAGES_PER_TABLE]
    __attribute__((aligned(PAGE_SIZE)));

/* Pointer to each directory entry's page table (for read-back). */
static uint32_t *page_tables[TABLES_PER_DIR];

/* How many tables from the static pool are in use. */
static uint32_t static_tables_used = 0;

/* Currently loaded page directory. */
static uint32_t *current_directory;

/* ------------------------------------------------------------------ */
/*  Inline helpers for CR registers                                   */
/* ------------------------------------------------------------------ */

static inline void write_cr3(uint32_t addr)
{
    asm volatile("mov %0, %%cr3" : : "r"(addr) : "memory");
}

static inline uint32_t read_cr2(void)
{
    uint32_t val;
    asm volatile("mov %%cr2, %0" : "=r"(val));
    return val;
}

static inline void enable_paging_bit(void)
{
    uint32_t cr0;
    asm volatile("mov %%cr0, %0" : "=r"(cr0));
    cr0 |= 0x80000000;          /* Set PG (bit 31) */
    asm volatile("mov %0, %%cr0" : : "r"(cr0) : "memory");
}

void paging_flush_tlb_entry(uint32_t addr)
{
    asm volatile("invlpg (%0)" : : "r"(addr) : "memory");
}

/* ------------------------------------------------------------------ */
/*  Internal: grab a page table from the static pool                  */
/* ------------------------------------------------------------------ */

static uint32_t *alloc_static_table(void)
{
    if (static_tables_used >= MAX_STATIC_TABLES)
        return 0;
    uint32_t *tbl = page_table_pool[static_tables_used++];
    memset(tbl, 0, PAGE_SIZE);
    return tbl;
}

/* ------------------------------------------------------------------ */
/*  Page-fault handler  (ISR 14)                                      */
/* ------------------------------------------------------------------ */

static void page_fault_handler(struct isr_regs *regs)
{
    uint32_t fault_addr = read_cr2();

    int not_present = !(regs->err_code & 0x1);
    int write       =   regs->err_code & 0x2;
    int user        =   regs->err_code & 0x4;
    int reserved    =   regs->err_code & 0x8;

    printf("\r\nPAGE FAULT at 0x");
    print_hex(fault_addr);
    printf(" (");
    if (not_present) printf("not-present ");
    if (write)       printf("write ");
    if (user)        printf("user-mode ");
    if (reserved)    printf("reserved-bit ");
    printf(") EIP=0x");
    print_hex(regs->eip);
    printf("\r\n");

    /* Extra diagnostic info */
    printf("  err_code=0x%x  cs=0x%x  ss=0x%x  useresp=0x%x\r\n",
           regs->err_code, regs->cs, regs->ss, regs->useresp);
    printf("  eax=0x%x  ebx=0x%x  ecx=0x%x  edx=0x%x\r\n",
           regs->eax, regs->ebx, regs->ecx, regs->edx);
    printf("  esi=0x%x  edi=0x%x  ebp=0x%x  ds=0x%x\r\n",
           regs->esi, regs->edi, regs->ebp, regs->ds);
    printf("  CR3=0x%x  current_dir=0x%x  kernel_pd=0x%x\r\n",
           (uint32_t)current_directory,
           (uint32_t)current_directory,
           (uint32_t)kernel_page_dir);

    /* Check if the faulting page is mapped in current directory */
    uint32_t dir_idx   = fault_addr >> 22;
    uint32_t table_idx = (fault_addr >> 12) & 0x3FF;
    printf("  PDE[%u]=0x%x", dir_idx, current_directory[dir_idx]);
    if (current_directory[dir_idx] & PDE_PRESENT) {
        uint32_t *pt = (uint32_t *)(current_directory[dir_idx] & 0xFFFFF000);
        printf("  PTE[%u]=0x%x", table_idx, pt[table_idx]);
    }
    printf("\r\n");

    panic();
}

/* ------------------------------------------------------------------ */
/*  Map / unmap helpers                                               */
/* ------------------------------------------------------------------ */

void paging_map_page(uint32_t virt, uint32_t phys, uint32_t flags)
{
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;

    /* Allocate a page table on first use of this 4 MiB region. */
    if (!(kernel_page_dir[dir_idx] & PDE_PRESENT)) {
        uint32_t *new_table = alloc_static_table();
        if (!new_table) {
            printf("[PANIC] paging: no free page tables\r\n");
            panic();
        }

        page_tables[dir_idx] = new_table;
        kernel_page_dir[dir_idx] = ((uint32_t)new_table)
                                   | PDE_PRESENT | PDE_WRITABLE;
    }

    page_tables[dir_idx][table_idx] =
        (phys & 0xFFFFF000) | (flags & 0xFFF) | PTE_PRESENT;
}

void paging_unmap_page(uint32_t virt)
{
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;

    if (!(kernel_page_dir[dir_idx] & PDE_PRESENT))
        return;

    page_tables[dir_idx][table_idx] = 0;
    paging_flush_tlb_entry(virt);
}

uint32_t paging_get_physical(uint32_t virt)
{
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;

    if (!(kernel_page_dir[dir_idx] & PDE_PRESENT))
        return 0;

    uint32_t entry = page_tables[dir_idx][table_idx];
    if (!(entry & PTE_PRESENT))
        return 0;

    return (entry & 0xFFFFF000) | (virt & 0xFFF);
}

uint32_t *paging_get_page_dir(void)
{
    return kernel_page_dir;
}

/* Number of kernel PD entries used for identity mapping. */
static uint32_t kernel_pd_entries_used = 0;

/* ------------------------------------------------------------------ */
/*  Per-process page directory support                                 */
/* ------------------------------------------------------------------ */

/*
 * Map a page in a specific page directory.
 * Allocates a new page table via the physical allocator if needed.
 */
void paging_map_page_in(uint32_t *pd, uint32_t virt, uint32_t phys,
                        uint32_t flags)
{
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;

    if (!(pd[dir_idx] & PDE_PRESENT)) {
        uint32_t *new_table = allocate_blocks(1);
        if (!new_table) {
            printf("[PANIC] paging_map_page_in: no memory for page table\r\n");
            panic();
        }
        memset(new_table, 0, PAGE_SIZE);
        pd[dir_idx] = ((uint32_t)new_table)
                       | PDE_PRESENT | PDE_WRITABLE | PDE_USER;
    }

    /* Identity mapping: PDE address field IS the virtual address of the table */
    uint32_t *pt = (uint32_t *)(pd[dir_idx] & 0xFFFFF000);
    pt[table_idx] = (phys & 0xFFFFF000) | (flags & 0xFFF) | PTE_PRESENT;
}

/*
 * Look up a physical address in a specific page directory.
 */
uint32_t paging_get_physical_in(uint32_t *pd, uint32_t virt)
{
    uint32_t dir_idx   = virt >> 22;
    uint32_t table_idx = (virt >> 12) & 0x3FF;

    if (!(pd[dir_idx] & PDE_PRESENT))
        return 0;

    uint32_t *pt = (uint32_t *)(pd[dir_idx] & 0xFFFFF000);
    uint32_t entry = pt[table_idx];
    if (!(entry & PTE_PRESENT))
        return 0;

    return (entry & 0xFFFFF000) | (virt & 0xFFF);
}

/*
 * Create a new page directory for a user process.
 * Kernel identity-map entries are shared (same page tables).
 */
uint32_t *paging_create_user_directory(void)
{
    uint32_t *new_dir = allocate_blocks(1);
    if (!new_dir)
        return NULL;

    memset(new_dir, 0, PAGE_SIZE);

    /* Copy all kernel PD entries (shared page tables). */
    for (uint32_t i = 0; i < TABLES_PER_DIR; i++) {
        if (kernel_page_dir[i] & PDE_PRESENT)
            new_dir[i] = kernel_page_dir[i];
    }

    return new_dir;
}

/*
 * Free a user-process page directory.
 * Frees per-process page tables (those NOT in the kernel PD).
 * Does NOT free user data pages — caller must do that first.
 */
void paging_free_user_directory(uint32_t *pd)
{
    if (!pd || pd == kernel_page_dir)
        return;

    for (uint32_t i = 0; i < TABLES_PER_DIR; i++) {
        if (!(pd[i] & PDE_PRESENT))
            continue;
        /* If this PD entry is also in the kernel PD, it's a shared table. */
        if (kernel_page_dir[i] & PDE_PRESENT) {
            uint32_t kern_pt = kernel_page_dir[i] & 0xFFFFF000;
            uint32_t user_pt = pd[i] & 0xFFFFF000;
            if (kern_pt == user_pt)
                continue;  /* shared kernel table — don't free */
        }
        /* Per-process page table: free it. */
        uint32_t *pt = (uint32_t *)(pd[i] & 0xFFFFF000);
        free_blocks(pt, 1);
    }

    free_blocks(pd, 1);
}

/*
 * Switch the active page directory (writes CR3).
 */
void paging_switch_directory(uint32_t *pd)
{
    if (pd != current_directory) {
        current_directory = pd;
        write_cr3((uint32_t)pd);
    }
}

/* ------------------------------------------------------------------ */
/*  Initialization                                                    */
/* ------------------------------------------------------------------ */

void paging_init(uint32_t mem_size_kb, uint32_t fb_phys, uint32_t fb_size)
{
    /*
     * Cap at the 32-bit address-space limit.  With >=4 GiB of physical
     * RAM, mem_size_kb * 1024 overflows uint32_t, so clamp first.
     */
    uint32_t mem_bytes;
    if (mem_size_kb >= 0x400000)               /* >= 4 GiB in KiB */
        mem_bytes = 0xFFFFF000;                /* ~4 GiB, page-aligned */
    else
        mem_bytes = mem_size_kb * 1024;

    if (mem_bytes == 0)
        mem_bytes = 16 * 1024 * 1024;          /* 16 MiB fallback */

    /* Clear the page directory and tracking arrays. */
    memset(kernel_page_dir, 0, sizeof(kernel_page_dir));
    memset(page_tables,     0, sizeof(page_tables));
    static_tables_used = 0;

    /*
     * Identity-map all physical memory (virtual == physical).
     * Each page table covers 4 MiB (1024 pages × 4 KiB).
     */
    uint32_t num_pages  = mem_bytes / PAGE_SIZE;
    uint32_t num_tables = (num_pages + PAGES_PER_TABLE - 1) / PAGES_PER_TABLE;
    if (num_tables > MAX_STATIC_TABLES)
        num_tables = MAX_STATIC_TABLES;

    for (uint32_t t = 0; t < num_tables; t++) {
        uint32_t *table = alloc_static_table();
        if (!table)
            break;                    /* pool exhausted */

        page_tables[t] = table;

        /* Fill every entry in this page table with an identity mapping. */
        for (uint32_t p = 0; p < PAGES_PER_TABLE; p++) {
            uint32_t phys = (t * PAGES_PER_TABLE + p) * PAGE_SIZE;
            if (phys >= mem_bytes)
                break;
            table[p] = phys | PTE_PRESENT | PTE_WRITABLE;
        }

        kernel_page_dir[t] = ((uint32_t)table) | PDE_PRESENT | PDE_WRITABLE;
    }

    kernel_pd_entries_used = num_tables;

    /* Identity-map the framebuffer (video RAM at a high physical address).
     * This must happen before enabling paging so the terminal keeps working. */
    if (fb_phys && fb_size) {
        uint32_t fb_start = fb_phys & ~(PAGE_SIZE - 1);
        uint32_t fb_end   = fb_phys + fb_size;
        for (uint32_t addr = fb_start; addr < fb_end; addr += PAGE_SIZE) {
            paging_map_page(addr, addr, PTE_WRITABLE);
        }
    }

    /* Register the page-fault handler before enabling paging. */
    isr_register_handler(14, page_fault_handler);

    /* Load the page directory into CR3 and flip the PG bit in CR0. */
    current_directory = kernel_page_dir;
    write_cr3((uint32_t)kernel_page_dir);
    enable_paging_bit();

    printf("Paging enabled: ");
    print_hex(mem_bytes);
    printf(" bytes identity-mapped\r\n");
}
