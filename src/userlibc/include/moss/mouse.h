/*
 * MOSS user-space mouse interface.
 * Non-blocking mouse event access via syscalls.
 */
#ifndef _MOSS_MOUSE_H
#define _MOSS_MOUSE_H

#include <stdint.h>

/* Mouse button bits */
#define MOUSE_BTN_LEFT   0x01
#define MOUSE_BTN_RIGHT  0x02
#define MOUSE_BTN_MIDDLE 0x04

typedef struct {
    int16_t  dx;        /* relative X movement */
    int16_t  dy;        /* relative Y movement */
    uint8_t  buttons;   /* MOUSE_BTN_* bitmask */
    uint8_t  _pad[3];
} mouse_event_t;

/*
 * moss_poll_mouse — non-blocking.
 * Returns 1 and fills *ev if an event is available, 0 otherwise.
 */
int moss_poll_mouse(mouse_event_t *ev);

/*
 * moss_get_mouse_pos — get absolute cursor position.
 * Fills *x and *y with screen coordinates.
 * Returns current button state (MOUSE_BTN_* bitmask).
 */
int moss_get_mouse_pos(int32_t *x, int32_t *y);

#endif /* _MOSS_MOUSE_H */
