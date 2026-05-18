#ifndef ARCH_I386_TIMER_H
#define ARCH_I386_TIMER_H

#include <stdint.h>

/* PIT (8253/8254) I/O ports */
#define PIT_CHANNEL0_DATA  0x40
#define PIT_COMMAND        0x43

/* PIT oscillator frequency (Hz) */
#define PIT_BASE_FREQ  1193182

/* Desired tick frequency (Hz) */
#define TIMER_FREQ  1000

/* Returns the number of ticks since boot. */
uint32_t timer_get_ticks(void);

/* Initialise the PIT and register the IRQ0 handler. */
void timer_init(uint32_t frequency);

#endif /* ARCH_I386_TIMER_H */
