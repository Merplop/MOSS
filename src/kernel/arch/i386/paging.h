#ifndef ARCH_I386_PAGING_H
#define ARCH_I386_PAGING_H

#include <stdint.h>

#define PAGE_SIZE        4096
#define PAGES_PER_TABLE  1024
#define TABLES_PER_DIR   1024

/* Page table entry flags */
#define PTE_PRESENT      0x001
#define PTE_WRITABLE     0x002
#define PTE_USER         0x004
#define PTE_WRITETHROUGH 0x008
#define PTE_CACHEDISABLE 0x010
#define PTE_ACCESSED     0x020
#define PTE_DIRTY        0x040

/* Page directory entry flags */
#define PDE_PRESENT      0x001
#define PDE_WRITABLE     0x002
#define PDE_USER         0x004
#define PDE_WRITETHROUGH 0x008
#define PDE_CACHEDISABLE 0x010
#define PDE_ACCESSED     0x020
#define PDE_PAGESIZE     0x080  /* 4 MiB pages (requires PSE) */

/* Initialize paging with identity mapping for mem_size_kb kilobytes. */
void paging_init(uint32_t mem_size_kb);

/* Map a single 4 KiB page: virtual_addr -> physical_addr with flags. */
void paging_map_page(uint32_t virtual_addr, uint32_t physical_addr,
                     uint32_t flags);

/* Remove the mapping for a virtual page. */
void paging_unmap_page(uint32_t virtual_addr);

/* Return the physical address for a mapped virtual address (0 if unmapped). */
uint32_t paging_get_physical(uint32_t virtual_addr);

/* Invalidate the TLB entry for a single page. */
void paging_flush_tlb_entry(uint32_t virtual_addr);

#endif /* ARCH_I386_PAGING_H */
