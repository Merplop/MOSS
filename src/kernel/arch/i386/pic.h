#ifndef ARCH_I386_PIC_H
#define ARCH_I386_PIC_H

#include <stdint.h>

/* I/O port addresses for the two 8259A PICs */
#define PIC1_COMMAND  0x20
#define PIC1_DATA     0x21
#define PIC2_COMMAND  0xA0
#define PIC2_DATA     0xA1

/* Default vector offsets after remapping */
#define PIC_OFFSET_MASTER  0x20   /* IRQ 0-7  => INT 32-39 */
#define PIC_OFFSET_SLAVE   0x28   /* IRQ 8-15 => INT 40-47 */

/* End-Of-Interrupt command byte */
#define PIC_EOI  0x20

/* ICW1 flags */
#define ICW1_ICW4       0x01
#define ICW1_INIT       0x10

/* ICW4 flags */
#define ICW4_8086       0x01

/* Remap both PICs so hardware IRQs don't collide with CPU exceptions. */
void pic_remap(uint8_t offset_master, uint8_t offset_slave);

/* Send End-Of-Interrupt for the given IRQ number (0-15). */
void pic_send_eoi(uint8_t irq);

/* Mask (disable) a specific IRQ line. */
void pic_set_mask(uint8_t irq);

/* Unmask (enable) a specific IRQ line. */
void pic_clear_mask(uint8_t irq);

#endif /* ARCH_I386_PIC_H */
