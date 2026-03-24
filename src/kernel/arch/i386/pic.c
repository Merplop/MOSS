/*
 * 8259A PIC (Programmable Interrupt Controller) driver.
 * MOSS Kernel – i386
 */

#include <stdint.h>
#include <sys/io.h>
#include "pic.h"

void pic_remap(uint8_t offset_master, uint8_t offset_slave) {
    uint8_t mask1, mask2;

    /* Save current masks */
    mask1 = inb(PIC1_DATA);
    mask2 = inb(PIC2_DATA);

    /* ICW1: begin initialisation in cascade mode */
    outb(PIC1_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();
    outb(PIC2_COMMAND, ICW1_INIT | ICW1_ICW4);
    io_wait();

    /* ICW2: vector offsets */
    outb(PIC1_DATA, offset_master);
    io_wait();
    outb(PIC2_DATA, offset_slave);
    io_wait();

    /* ICW3: tell master that slave is on IRQ2, tell slave its cascade id */
    outb(PIC1_DATA, 0x04);   /* slave on IRQ2 */
    io_wait();
    outb(PIC2_DATA, 0x02);   /* cascade identity */
    io_wait();

    /* ICW4: 8086 mode */
    outb(PIC1_DATA, ICW4_8086);
    io_wait();
    outb(PIC2_DATA, ICW4_8086);
    io_wait();

    /* Restore saved masks */
    outb(PIC1_DATA, mask1);
    outb(PIC2_DATA, mask2);
}

void pic_send_eoi(uint8_t irq) {
    if (irq >= 8)
        outb(PIC2_COMMAND, PIC_EOI);
    outb(PIC1_COMMAND, PIC_EOI);
}

void pic_set_mask(uint8_t irq) {
    uint16_t port;
    uint8_t  value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = inb(port) | (1 << irq);
    outb(port, value);
}

void pic_clear_mask(uint8_t irq) {
    uint16_t port;
    uint8_t  value;

    if (irq < 8) {
        port = PIC1_DATA;
    } else {
        port = PIC2_DATA;
        irq -= 8;
    }
    value = inb(port) & ~(1 << irq);
    outb(port, value);
}
