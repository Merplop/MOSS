#ifndef ARCH_I386_IDT_H
#define ARCH_I386_IDT_H

#include <stdint.h>

#define IDT_ENTRIES 256

/* IDT gate types */
#define IDT_GATE_INTERRUPT  0x8E  /* P=1, DPL=0, 32-bit interrupt gate */
#define IDT_GATE_TRAP       0x8F  /* P=1, DPL=0, 32-bit trap gate     */
#define IDT_GATE_SYSCALL    0xEE  /* P=1, DPL=3, 32-bit interrupt gate */

/* An entry in the Interrupt Descriptor Table. */
struct idt_entry {
    uint16_t base_lo;   /* Lower 16 bits of handler address */
    uint16_t sel;       /* Kernel code segment selector     */
    uint8_t  always0;   /* Must be zero                     */
    uint8_t  flags;     /* Gate type, DPL, and present bit  */
    uint16_t base_hi;   /* Upper 16 bits of handler address */
} __attribute__((packed));

/* Pointer structure passed to lidt. */
struct idt_ptr {
    uint16_t limit;
    uint32_t base;
} __attribute__((packed));

/* Saved CPU registers pushed by the ISR stub. */
struct isr_regs {
    uint32_t ds;
    uint32_t edi, esi, ebp, esp, ebx, edx, ecx, eax;  /* pusha */
    uint32_t int_no, err_code;
    uint32_t eip, cs, eflags, useresp, ss;              /* pushed by CPU */
};

/* Initialise the IDT and load it with lidt. */
void idt_init(void);

/* Set a single gate in the IDT. */
void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags);

/* Register a C handler for a given interrupt number. */
typedef void (*isr_handler_t)(struct isr_regs *regs);
void isr_register_handler(uint8_t n, isr_handler_t handler);

/*
 * ISR stubs (defined in isr.S).
 * 0-31  : CPU exceptions
 * 32-47 : Hardware IRQs (PIC remapped)
 */
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);

extern void irq0(void);
extern void irq1(void);
extern void irq2(void);
extern void irq3(void);
extern void irq4(void);
extern void irq5(void);
extern void irq6(void);
extern void irq7(void);
extern void irq8(void);
extern void irq9(void);
extern void irq10(void);
extern void irq11(void);
extern void irq12(void);
extern void irq13(void);
extern void irq14(void);
extern void irq15(void);

extern void isr128(void);   /* syscall vector (int 0x80) */

#endif /* ARCH_I386_IDT_H */
