#ifndef ARCH_I386_SB16_H
#define ARCH_I386_SB16_H

#include <stdint.h>

/* Detect and initialize the Sound Blaster 16.
 * Returns 0 on success, -1 if not found. */
int sb16_init(void);

/* Begin playback at the given sample rate (unsigned 8-bit mono).
 * Call after sb16_init().  Starts DMA auto-init loop. */
void sb16_start(uint32_t sample_rate);

/* Stop DMA playback. */
void sb16_stop(void);

/* Write audio samples into the ring buffer.
 * Returns number of bytes actually written. */
uint32_t sb16_write(const uint8_t *buf, uint32_t len);

/* Query free space (bytes) in the ring buffer. */
uint32_t sb16_avail(void);

#endif /* ARCH_I386_SB16_H */
