/*
 * PS/2 Mouse driver — IRQ12-driven with event queue.
 * MOSS Kernel
 */
#ifndef _KERNEL_MOUSE_H
#define _KERNEL_MOUSE_H

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

/* Initialize the PS/2 mouse (IRQ12). Call after keyboard_init(). */
void mouse_init(void);

/* Non-blocking: returns 1 and fills *ev if event available, 0 otherwise. */
int mouse_poll_event(mouse_event_t *out);

/* Get absolute cursor position (clamped to screen). */
void mouse_get_position(int32_t *x, int32_t *y);

/* Get current button state. */
uint8_t mouse_get_buttons(void);

/* Set screen bounds for clamping (call after framebuffer is known). */
void mouse_set_bounds(int32_t width, int32_t height);

#endif /* _KERNEL_MOUSE_H */
