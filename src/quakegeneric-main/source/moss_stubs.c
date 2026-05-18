/*
 * MOSS-specific syscall wrappers for quakegeneric.
 * These were previously provided by userlibc.
 */
#include <moss/fb.h>
#include <moss/keyboard.h>
#include <syscall.h>

uint32_t moss_fb_map(moss_fbinfo_t *info) {
    return (uint32_t)_syscall1(SYS_FBMAP, (uint32_t)info);
}

void moss_fb_flush(void) {
    _syscall0(SYS_FB_FLUSH);
}

int moss_poll_key(key_event_t *ev) {
    return (int)_syscall1(SYS_POLL_KEY, (uint32_t)ev);
}

unsigned int getticks(void) {
    return (unsigned int)_syscall0(SYS_GETTICKS);
}
