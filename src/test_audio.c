/*
 * test_audio — Sound Blaster 16 test program for MOSS.
 *
 * Generates a 440 Hz sine wave (A4 note) for ~2 seconds using
 * unsigned 8-bit mono PCM at 11025 Hz, then a 880 Hz tone for 1 second.
 */
#include <stdio.h>
#include <unistd.h>
#include <moss/audio.h>

#define SAMPLE_RATE  11025
#define DURATION_MS  2000
#define DURATION2_MS 1000
#define CHUNK_SIZE   512

/* Simple fixed-point sine approximation (no libm needed).
 * Input: angle in units of 1/256 of a full cycle (0-255).
 * Output: value in range -127..+127 (approximately). */
static int fast_sin(unsigned int phase256)
{
    /* Use a parabolic approximation of sine.
     * Map phase 0-255 to quadrant and compute. */
    int x = (int)(phase256 & 255);

    /* Normalize to -128..+127 range centered on zero crossing */
    if (x < 64)
        return x * 2;           /* 0..127 */
    else if (x < 192)
        return (128 - x) * 2;  /* 127..-127 */
    else
        return (x - 256) * 2;  /* -127..0 */
}

static void play_tone(unsigned int freq_hz, unsigned int duration_ms)
{
    unsigned int total_samples = (SAMPLE_RATE * duration_ms) / 1000;
    unsigned int samples_sent = 0;
    /* phase accumulator: fixed point 16.16
     * phase_inc = freq * 256 * 65536 / SAMPLE_RATE */
    unsigned int phase_acc = 0;
    unsigned int phase_inc = (freq_hz * 256 * 65536u) / SAMPLE_RATE;

    uint8_t buf[CHUNK_SIZE];

    while (samples_sent < total_samples) {
        /* Wait until there's space in the ring buffer */
        while (moss_audio_avail() < CHUNK_SIZE)
            usleep(5);

        unsigned int to_gen = total_samples - samples_sent;
        if (to_gen > CHUNK_SIZE)
            to_gen = CHUNK_SIZE;

        for (unsigned int i = 0; i < to_gen; i++) {
            int phase256 = (phase_acc >> 16) & 255;
            int sample = fast_sin(phase256);
            /* Convert signed -127..+127 to unsigned 0..255 centered at 128 */
            buf[i] = (uint8_t)(128 + sample);
            phase_acc += phase_inc;
        }

        unsigned int written = moss_audio_write(buf, to_gen);
        samples_sent += written;
    }
}

int main(void)
{
    printf("SB16 Audio Test\n");
    printf("Starting playback at %d Hz...\n", SAMPLE_RATE);

    moss_audio_start(SAMPLE_RATE);

    printf("Playing 440 Hz tone for 2 seconds...\n");
    play_tone(440, DURATION_MS);

    printf("Playing 880 Hz tone for 1 second...\n");
    play_tone(880, DURATION2_MS);

    /* Let the buffer drain */
    printf("Waiting for playback to finish...\n");
    usleep(1000);

    printf("Done!\n");
    return 0;
}
