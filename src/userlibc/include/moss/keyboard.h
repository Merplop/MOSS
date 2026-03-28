/*
 * MOSS user-space keyboard interface.
 * Non-blocking and blocking key event access via syscalls.
 */
#ifndef _MOSS_KEYBOARD_H
#define _MOSS_KEYBOARD_H

#include <stdint.h>

/* Key event flags */
#define KEY_EVENT_PRESS    0x01
#define KEY_EVENT_RELEASE  0x02
#define KEY_EVENT_EXTENDED 0x04

/* Modifier bits */
#define KEY_MOD_SHIFT    0x01
#define KEY_MOD_CTRL     0x02
#define KEY_MOD_ALT      0x04
#define KEY_MOD_CAPSLOCK 0x08

typedef struct {
    uint8_t scancode;   /* raw scancode (0-127) */
    uint8_t flags;      /* KEY_EVENT_PRESS | KEY_EVENT_RELEASE | KEY_EVENT_EXTENDED */
    uint8_t ascii;      /* ASCII translation (0 if none) */
    uint8_t modifiers;  /* KEY_MOD_* bitmask at time of event */
} key_event_t;

/*
 * moss_poll_key — non-blocking.
 * Returns 1 and fills *ev if an event is available, 0 otherwise.
 */
int moss_poll_key(key_event_t *ev);

/*
 * moss_wait_key — blocking.
 * Waits until a key event is available, then fills *ev.
 * Returns 1.
 */
int moss_wait_key(key_event_t *ev);

#endif /* _MOSS_KEYBOARD_H */
