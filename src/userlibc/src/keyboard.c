/*
 * MOSS user-space keyboard syscall wrappers.
 */
#include <moss/keyboard.h>
#include <syscall.h>

int moss_poll_key(key_event_t *ev) {
    return (int)_syscall1(SYS_POLL_KEY, (uint32_t)ev);
}

int moss_wait_key(key_event_t *ev) {
    return (int)_syscall1(SYS_WAIT_KEY, (uint32_t)ev);
}
