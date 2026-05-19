#ifndef _KERNEL_KEYBOARD_H
#define _KERNEL_KEYBOARD_H

#include <stdint.h>

/* ------------------------------------------------------------------ */
/*  Scancode constants                                                 */
/* ------------------------------------------------------------------ */

#define LSHIFT_MAKE     0x2A
#define LSHIFT_BREAK    0xAA
#define RSHIFT_MAKE     0x36
#define RSHIFT_BREAK    0xB6
#define LCTRL_MAKE      0x1D
#define LCTRL_BREAK     0x9D
#define CAPS_LOCK_MAKE  0x3A

/* ------------------------------------------------------------------ */
/*  Key event structure                                                */
/* ------------------------------------------------------------------ */

/* Flags for key_event_t.flags */
#define KEY_EVENT_PRESS    0x01   /* key pressed */
#define KEY_EVENT_RELEASE  0x02   /* key released */
#define KEY_EVENT_EXTENDED 0x04   /* extended scancode (0xE0 prefix) */

typedef struct {
    uint8_t scancode;     /* raw PS/2 scancode (without 0xE0 prefix or 0x80 bit) */
    uint8_t flags;        /* KEY_EVENT_PRESS, KEY_EVENT_RELEASE, KEY_EVENT_EXTENDED */
    uint8_t ascii;        /* translated ASCII character (0 if non-printable) */
    uint8_t modifiers;    /* current modifier state (KEY_MOD_*) */
} key_event_t;

/* Modifier bits for key_event_t.modifiers */
#define KEY_MOD_SHIFT     0x01
#define KEY_MOD_CTRL      0x02
#define KEY_MOD_ALT       0x04
#define KEY_MOD_CAPSLOCK  0x08

/* ------------------------------------------------------------------ */
/*  API                                                                */
/* ------------------------------------------------------------------ */

/* Initialise the keyboard driver (registers IRQ1 handler, unmasks IRQ1). */
void keyboard_init(void);

/* Blocking: wait for and return the next ASCII character (for the shell). */
uint8_t get_key(void);

/* Non-blocking: poll the next key event.
 * Returns 1 if an event was available (copied into *out), 0 if queue empty. */
int keyboard_poll_event(key_event_t *out);

/* Check if there are any key events pending in the queue (non-destructive). */
int keyboard_has_events(void);

/* Check if there's a key-press event with ASCII (for TTY readability). */
int keyboard_has_ascii_press(void);

/* Query the current modifier key state. */
uint8_t keyboard_get_modifiers(void);

/* Query whether a specific scancode is currently held down.
 * scancode: 7-bit raw scancode (use 0x80|sc for extended keys). */
int keyboard_is_pressed(uint8_t scancode);

#endif /* _KERNEL_KEYBOARD_H */
