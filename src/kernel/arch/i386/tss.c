/*
 * TSS and GDT management for i386.
 * MOSS Kernel
 *
 * The GDT is extended at runtime from the 3-entry table set up in boot.S
 * to include ring-3 code/data segments and a TSS descriptor.
 *
 * GDT layout (after tss_init):
 *   0x00  Null
 *   0x08  Kernel code  (ring 0)
 *   0x10  Kernel data  (ring 0)
 *   0x18  User code    (ring 3)
 *   0x20  User data    (ring 3)
 *   0x28  TSS
 */

#include <stdint.h>
#include <string.h>
#include "tss.h"

/* ------------------------------------------------------------------ */
/*  GDT                                                                */
/* ------------------------------------------------------------------ */

struct gdt_entry {
    uint16_t limit_lo;
    uint16_t base_lo;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_hi;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* null + kcode + kdata + ucode + udata + tss + 3 TLS entries */
#define GDT_ENTRIES 9

static struct gdt_entry gdt[GDT_ENTRIES];
static struct gdt_ptr   gdtp;
static struct tss_entry tss;

static void gdt_set_gate(int num, uint32_t base, uint32_t limit,
                          uint8_t access, uint8_t gran) {
    gdt[num].base_lo     = (uint16_t)(base & 0xFFFF);
    gdt[num].base_mid    = (uint8_t)((base >> 16) & 0xFF);
    gdt[num].base_hi     = (uint8_t)((base >> 24) & 0xFF);
    gdt[num].limit_lo    = (uint16_t)(limit & 0xFFFF);
    gdt[num].granularity = (uint8_t)((gran & 0xF0) | ((limit >> 16) & 0x0F));
    gdt[num].access      = access;
}

/*
 * Reload all segment selectors after loading a new GDT.
 * The far jump reloads CS; the mov instructions reload the data selectors.
 */
static void gdt_flush(void) {
    asm volatile(
        "lgdt %0\n\t"
        "ljmp $0x08, $.Lflush_cs\n\t"
        ".Lflush_cs:\n\t"
        "movw $0x10, %%ax\n\t"
        "movw %%ax, %%ds\n\t"
        "movw %%ax, %%es\n\t"
        "movw %%ax, %%fs\n\t"
        "movw %%ax, %%gs\n\t"
        "movw %%ax, %%ss\n\t"
        :
        : "m"(gdtp)
        : "eax", "memory"
    );
}

/* Load the TSS selector into the task register. */
static void tss_flush(void) {
    asm volatile(
        "movw $0x28, %%ax\n\t"
        "ltr  %%ax\n\t"
        ::: "eax"
    );
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void tss_init(uint32_t kernel_ss, uint32_t kernel_esp0) {
    /* Build the full GDT from scratch (replaces the minimal boot.S one) */

    /* 0: Null descriptor */
    gdt_set_gate(0, 0, 0, 0, 0);

    /* 1 (0x08): Kernel code — base 0, limit 4 GiB, ring 0, exec/read */
    gdt_set_gate(1, 0, 0xFFFFFFFF, 0x9A, 0xCF);

    /* 2 (0x10): Kernel data — base 0, limit 4 GiB, ring 0, read/write */
    gdt_set_gate(2, 0, 0xFFFFFFFF, 0x92, 0xCF);

    /* 3 (0x18): User code — base 0, limit 4 GiB, ring 3, exec/read */
    gdt_set_gate(3, 0, 0xFFFFFFFF, 0xFA, 0xCF);

    /* 4 (0x20): User data — base 0, limit 4 GiB, ring 3, read/write */
    gdt_set_gate(4, 0, 0xFFFFFFFF, 0xF2, 0xCF);

    /* 5 (0x28): TSS descriptor */
    uint32_t tss_base  = (uint32_t)&tss;
    uint32_t tss_limit = sizeof(tss) - 1;
    /* Access byte for TSS: present, ring 0, type = 0x9 (available 32-bit TSS) */
    gdt_set_gate(5, tss_base, tss_limit, 0x89, 0x00);

    /* 6-8 (0x30, 0x38, 0x40): TLS entries — initially null */
    gdt_set_gate(6, 0, 0, 0, 0);
    gdt_set_gate(7, 0, 0, 0, 0);
    gdt_set_gate(8, 0, 0, 0, 0);

    /* Set up the GDT pointer and load it */
    gdtp.limit = sizeof(gdt) - 1;
    gdtp.base  = (uint32_t)&gdt;
    gdt_flush();

    /* Initialise the TSS */
    memset(&tss, 0, sizeof(tss));
    tss.ss0  = kernel_ss;
    tss.esp0 = kernel_esp0;
    /* I/O map base beyond TSS limit = no I/O bitmap */
    tss.iomap_base = sizeof(tss);

    /* Load the task register */
    tss_flush();
}

void tss_set_kernel_stack(uint32_t esp0) {
    tss.esp0 = esp0;
}

/*
 * Set a TLS GDT entry for set_thread_area.
 * entry_number: 6, 7, or 8 (corresponding to GDT slots 6-8).
 * base: linear base address of the TLS block.
 * limit: segment limit (usually 0xFFFFF with 4K granularity for full 4GB).
 *
 * Sets up a ring-3 read/write data segment with the given base.
 * Returns the GDT index used, or -1 on error.
 */
int gdt_set_tls(int entry_number, uint32_t base, uint32_t limit) {
    /* Linux TLS entries are GDT indices 6, 7, 8 */
    if (entry_number < 6 || entry_number > 8)
        return -1;

    /* Access: present=1, DPL=3, type=data r/w (0xF2) */
    /* Granularity: 4K pages, 32-bit (0xCF upper nibble) */
    if (limit == 0 && base == 0) {
        /* Mark entry as not present (empty) */
        gdt_set_gate(entry_number, 0, 0, 0, 0);
    } else {
        gdt_set_gate(entry_number, base, limit, 0xF2, 0xCF);
    }

    return entry_number;
}

/*
 * Reload GS with a specific TLS selector.
 * Called after updating a TLS GDT entry.
 */
void gdt_load_gs(uint16_t selector) {
    asm volatile("movw %0, %%gs" : : "r"(selector));
}
