/*
 * PIT (Programmable Interval Timer) driver and IRQ0 handler.
 * MOSS Kernel – i386
 *
 * Configures PIT channel 0 in rate-generator mode (mode 2) to fire
 * at the requested frequency.  Each tick calls into the scheduler's
 * timer_tick() so the round-robin quantum is decremented.
 */

#include <stdint.h>
#include <sys/io.h>
#include <kernel/sched.h>
#include "idt.h"
#include "pic.h"
#include "timer.h"

static uint32_t tick_count = 0;

uint32_t timer_get_ticks(void) {
    return tick_count;
}

/* IRQ0 handler — called from the IDT dispatcher. */
static void timer_irq_handler(struct isr_regs *regs) {
    (void)regs;
    tick_count++;
    timer_tick();   /* decrement quantum, reschedule if expired */
}

void timer_init(uint32_t frequency) {
    /* Calculate the PIT divisor */
    uint32_t divisor = PIT_BASE_FREQ / frequency;
    if (divisor > 0xFFFF)
        divisor = 0xFFFF;
    if (divisor < 1)
        divisor = 1;

    /*
     * Command byte: channel 0, access lo/hi byte, mode 2 (rate generator)
     * Bits: 00 11 010 0  =  0x34
     */
    outb(PIT_COMMAND, 0x34);
    outb(PIT_CHANNEL0_DATA, (uint8_t)(divisor & 0xFF));        /* low byte  */
    outb(PIT_CHANNEL0_DATA, (uint8_t)((divisor >> 8) & 0xFF)); /* high byte */

    /* Register our handler for IRQ0 (vector 32) and unmask it */
    isr_register_handler(32, timer_irq_handler);
    pic_clear_mask(0);
}
