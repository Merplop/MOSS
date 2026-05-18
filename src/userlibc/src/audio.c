/*
 * MOSS user-space libc — audio output.
 */
#include <moss/audio.h>
#include <syscall.h>

void moss_audio_start(uint32_t sample_rate) {
    _syscall1(SYS_AUDIO_START, sample_rate);
}

uint32_t moss_audio_write(const uint8_t *buf, uint32_t len) {
    return (uint32_t)_syscall2(SYS_AUDIO_WRITE, (uint32_t)buf, len);
}

uint32_t moss_audio_avail(void) {
    return (uint32_t)_syscall0(SYS_AUDIO_AVAIL);
}
