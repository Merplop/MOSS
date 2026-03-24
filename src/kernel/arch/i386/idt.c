/*
 * IDT initialisation and C-level interrupt dispatcher.
 * MOSS Kernel – i386
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <kernel/kernel.h>
#include "idt.h"
#include "pic.h"

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr   idtp;

/* Table of registered C handlers (one per vector). */
static isr_handler_t isr_handlers[IDT_ENTRIES] = {0};

void idt_set_gate(uint8_t num, uint32_t base, uint16_t sel, uint8_t flags) {
    idt[num].base_lo = (uint16_t)(base & 0xFFFF);
    idt[num].base_hi = (uint16_t)((base >> 16) & 0xFFFF);
    idt[num].sel     = sel;
    idt[num].always0 = 0;
    idt[num].flags   = flags;
}

void isr_register_handler(uint8_t n, isr_handler_t handler) {
    isr_handlers[n] = handler;
}

/*
 * Common C handler called from the assembly stub.
 * For IRQs (int_no 32-47) we send the EOI to the PIC.
 */
void isr_handler(struct isr_regs *regs) {
    /*
     * Send EOI early for hardware IRQs.  This is necessary because
     * the timer handler may context-switch away via schedule(),
     * and the interrupted task's call chain won't return through
     * here until it is scheduled again.  Sending EOI first ensures
     * the PIC can deliver further interrupts.
     */
    if (regs->int_no >= 32 && regs->int_no < 48) {
        pic_send_eoi((uint8_t)(regs->int_no - 32));
    }

    if (isr_handlers[regs->int_no]) {
        isr_handlers[regs->int_no](regs);
    } else if (regs->int_no < 32) {
        /* Unhandled CPU exception — halt */
        printf("EXCEPTION: int ");
        /* simple decimal print */
        char buf[4];
        int n = regs->int_no;
        int i = 0;
        if (n == 0) { buf[i++] = '0'; }
        else {
            char tmp[4]; int j = 0;
            while (n) { tmp[j++] = '0' + (n % 10); n /= 10; }
            while (j) buf[i++] = tmp[--j];
        }
        buf[i] = '\0';
        printf(buf);
        printf(" err=");
        /* hex print of error code */
        print_hex(regs->err_code);
        printf("\r\n");
        panic();
    }
}

void idt_init(void) {
    idtp.limit = sizeof(idt) - 1;
    idtp.base  = (uint32_t)&idt;
    memset(&idt, 0, sizeof(idt));

    /* Remap the PIC first so IRQ 0-15 => INT 32-47 */
    pic_remap(PIC_OFFSET_MASTER, PIC_OFFSET_SLAVE);

    /* CPU exceptions 0-31 */
    idt_set_gate(0,  (uint32_t)isr0,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(1,  (uint32_t)isr1,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(2,  (uint32_t)isr2,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(3,  (uint32_t)isr3,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(4,  (uint32_t)isr4,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(5,  (uint32_t)isr5,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(6,  (uint32_t)isr6,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(7,  (uint32_t)isr7,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(8,  (uint32_t)isr8,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(9,  (uint32_t)isr9,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(10, (uint32_t)isr10, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(11, (uint32_t)isr11, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(12, (uint32_t)isr12, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(13, (uint32_t)isr13, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(14, (uint32_t)isr14, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(15, (uint32_t)isr15, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(16, (uint32_t)isr16, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(17, (uint32_t)isr17, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(18, (uint32_t)isr18, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(19, (uint32_t)isr19, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(20, (uint32_t)isr20, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(21, (uint32_t)isr21, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(22, (uint32_t)isr22, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(23, (uint32_t)isr23, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(24, (uint32_t)isr24, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(25, (uint32_t)isr25, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(26, (uint32_t)isr26, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(27, (uint32_t)isr27, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(28, (uint32_t)isr28, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(29, (uint32_t)isr29, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(30, (uint32_t)isr30, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(31, (uint32_t)isr31, 0x08, IDT_GATE_INTERRUPT);

    /* Hardware IRQs 0-15 => vectors 32-47 */
    idt_set_gate(32, (uint32_t)irq0,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(33, (uint32_t)irq1,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(34, (uint32_t)irq2,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(35, (uint32_t)irq3,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(36, (uint32_t)irq4,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(37, (uint32_t)irq5,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(38, (uint32_t)irq6,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(39, (uint32_t)irq7,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(40, (uint32_t)irq8,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(41, (uint32_t)irq9,  0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(42, (uint32_t)irq10, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(43, (uint32_t)irq11, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(44, (uint32_t)irq12, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(45, (uint32_t)irq13, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(46, (uint32_t)irq14, 0x08, IDT_GATE_INTERRUPT);
    idt_set_gate(47, (uint32_t)irq15, 0x08, IDT_GATE_INTERRUPT);

    /* Load the IDT */
    asm volatile ("lidt %0" : : "m"(idtp));
}
