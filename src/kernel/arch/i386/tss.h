#ifndef ARCH_I386_TSS_H
#define ARCH_I386_TSS_H

#include <stdint.h>

/*
 * i386 Task State Segment.
 * We only use the TSS for its esp0/ss0 fields so the CPU knows where
 * to find the kernel stack when an interrupt fires in ring 3.
 */
struct tss_entry {
    uint32_t prev_tss;
    uint32_t esp0;      /* kernel stack pointer */
    uint32_t ss0;       /* kernel stack segment (0x10) */
    uint32_t esp1;
    uint32_t ss1;
    uint32_t esp2;
    uint32_t ss2;
    uint32_t cr3;
    uint32_t eip;
    uint32_t eflags;
    uint32_t eax, ecx, edx, ebx;
    uint32_t esp, ebp, esi, edi;
    uint32_t es, cs, ss, ds, fs, gs;
    uint32_t ldt;
    uint16_t trap;
    uint16_t iomap_base;
} __attribute__((packed));

/* GDT segment selectors */
#define GDT_KERNEL_CODE  0x08
#define GDT_KERNEL_DATA  0x10
#define GDT_USER_CODE    0x1B   /* 0x18 | RPL 3 */
#define GDT_USER_DATA    0x23   /* 0x20 | RPL 3 */
#define GDT_TSS_SEG      0x28

/* Initialise the TSS and install its descriptor in the GDT.
 * Must be called after the GDT is loaded but before any user-mode switch. */
void tss_init(uint32_t kernel_ss, uint32_t kernel_esp0);

/* Update esp0 in the TSS (called on every context switch). */
void tss_set_kernel_stack(uint32_t esp0);

#endif /* ARCH_I386_TSS_H */
