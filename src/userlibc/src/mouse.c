/*
 * MOSS user-space mouse syscall wrappers.
 */
#include <moss/mouse.h>
#include <syscall.h>

#define SYS_POLL_MOUSE    513
#define SYS_GET_MOUSE_POS 514

int moss_poll_mouse(mouse_event_t *ev) {
    return (int)_syscall1(SYS_POLL_MOUSE, (uint32_t)ev);
}

int moss_get_mouse_pos(int32_t *x, int32_t *y) {
    int32_t pos[2];
    int ret = (int)_syscall1(SYS_GET_MOUSE_POS, (uint32_t)pos);
    if (x) *x = pos[0];
    if (y) *y = pos[1];
    return ret;
}
