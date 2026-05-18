/* <moss/audio.h> — Audio output for MOSS user-space programs */
#ifndef _MOSS_AUDIO_H
#define _MOSS_AUDIO_H

#include <stdint.h>

/* Start audio playback at the given sample rate (unsigned 8-bit mono). */
void moss_audio_start(uint32_t sample_rate);

/* Write unsigned 8-bit PCM samples to the audio ring buffer.
 * Returns the number of bytes actually accepted. */
uint32_t moss_audio_write(const uint8_t *buf, uint32_t len);

/* Query how many bytes of free space remain in the audio buffer. */
uint32_t moss_audio_avail(void);

#endif /* _MOSS_AUDIO_H */
