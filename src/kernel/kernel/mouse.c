/*
 * PS/2 Mouse driver — IRQ12-driven with event queue.
 * MOSS Kernel
 *
 * Initializes the PS/2 auxiliary device via the i8042 controller,
 * registers IRQ12, and provides a polling interface similar to keyboard.c.
 */

#include <stdint.h>
#include <kernel/mouse.h>

/* Forward declarations for arch-specific functions */
struct isr_regs;
typedef void (*isr_handler_t)(struct isr_regs *regs);
void isr_register_handler(uint8_t n, isr_handler_t handler);
void pic_clear_mask(uint8_t irq);

/* ------------------------------------------------------------------ */
/*  i8042 controller helpers                                           */
/* ------------------------------------------------------------------ */

#define I8042_DATA    0x60
#define I8042_STATUS  0x64
#define I8042_CMD     0x64

static inline uint8_t inb(uint16_t port) {
    uint8_t val;
    asm volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

/* Wait until the i8042 input buffer is empty (ready to accept a command). */
static void i8042_wait_write(void) {
    int timeout = 100000;
    while ((inb(I8042_STATUS) & 0x02) && --timeout > 0)
        ;
}

/* Wait until the i8042 output buffer is full (data available to read). */
static void i8042_wait_read(void) {
    int timeout = 100000;
    while (!(inb(I8042_STATUS) & 0x01) && --timeout > 0)
        ;
}

/* Send a command byte to the i8042 controller. */
static void i8042_send_cmd(uint8_t cmd) {
    i8042_wait_write();
    outb(I8042_CMD, cmd);
}

/* Send a byte to the mouse (via i8042 auxiliary port). */
static void mouse_write(uint8_t data) {
    i8042_send_cmd(0xD4);  /* Tell controller: next byte goes to aux device */
    i8042_wait_write();
    outb(I8042_DATA, data);
}

/* Read a response byte from the mouse. */
static uint8_t mouse_read(void) {
    i8042_wait_read();
    return inb(I8042_DATA);
}

/* ------------------------------------------------------------------ */
/*  Event ring buffer                                                  */
/* ------------------------------------------------------------------ */

#define MOUSE_QUEUE_SIZE 64  /* must be power of 2 */

static mouse_event_t event_queue[MOUSE_QUEUE_SIZE];
static volatile uint32_t eq_head = 0;
static volatile uint32_t eq_tail = 0;

static void eq_push(const mouse_event_t *ev) {
    uint32_t next = (eq_head + 1) & (MOUSE_QUEUE_SIZE - 1);
    if (next == eq_tail)
        return;  /* queue full, drop event */
    event_queue[eq_head] = *ev;
    eq_head = next;
}

static int eq_pop(mouse_event_t *out) {
    if (eq_tail == eq_head)
        return 0;  /* empty */
    *out = event_queue[eq_tail];
    eq_tail = (eq_tail + 1) & (MOUSE_QUEUE_SIZE - 1);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Absolute position tracking (clamped to screen)                     */
/* ------------------------------------------------------------------ */

static int32_t mouse_x = 0;
static int32_t mouse_y = 0;
static int32_t screen_w = 1920;
static int32_t screen_h = 1080;
static uint8_t current_buttons = 0;

void mouse_set_bounds(int32_t width, int32_t height) {
    screen_w = width;
    screen_h = height;
    /* Clamp current position */
    if (mouse_x >= screen_w) mouse_x = screen_w - 1;
    if (mouse_y >= screen_h) mouse_y = screen_h - 1;
}

void mouse_get_position(int32_t *x, int32_t *y) {
    if (x) *x = mouse_x;
    if (y) *y = mouse_y;
}

uint8_t mouse_get_buttons(void) {
    return current_buttons;
}

/* ------------------------------------------------------------------ */
/*  IRQ12 handler — parse PS/2 mouse packets                           */
/* ------------------------------------------------------------------ */

static uint8_t mouse_cycle = 0;   /* which byte of the 3-byte packet */
static uint8_t mouse_bytes[3];    /* accumulated packet bytes */

static void mouse_irq_handler(struct isr_regs *regs) {
    (void)regs;

    /* Only read if output buffer is full and aux bit is set */
    uint8_t status = inb(I8042_STATUS);
    if (!(status & 0x01))
        return;

    uint8_t data = inb(I8042_DATA);

    /* If bit 5 of status is not set, this is keyboard data, ignore */
    if (!(status & 0x20))
        return;

    switch (mouse_cycle) {
    case 0:
        /* First byte: must have bit 3 set (always-1 in PS/2 protocol) */
        if (!(data & 0x08)) {
            /* Out of sync — discard and wait for valid first byte */
            return;
        }
        mouse_bytes[0] = data;
        mouse_cycle = 1;
        break;
    case 1:
        mouse_bytes[1] = data;
        mouse_cycle = 2;
        break;
    case 2:
        mouse_bytes[2] = data;
        mouse_cycle = 0;

        /* Decode the 3-byte packet */
        uint8_t flags = mouse_bytes[0];
        int16_t dx = (int16_t)mouse_bytes[1];
        int16_t dy = (int16_t)mouse_bytes[2];

        /* Apply sign extension from flags byte */
        if (flags & 0x10) dx |= 0xFF00;  /* X sign bit */
        if (flags & 0x20) dy |= 0xFF00;  /* Y sign bit */

        /* Discard overflow packets */
        if (flags & 0x40 || flags & 0x80)
            return;

        /* PS/2 Y axis is inverted (positive = up), flip for screen coords */
        dy = -dy;

        /* Update absolute position */
        mouse_x += dx;
        mouse_y += dy;

        /* Clamp to screen bounds */
        if (mouse_x < 0) mouse_x = 0;
        if (mouse_y < 0) mouse_y = 0;
        if (mouse_x >= screen_w) mouse_x = screen_w - 1;
        if (mouse_y >= screen_h) mouse_y = screen_h - 1;

        /* Update button state */
        current_buttons = flags & 0x07;

        /* Push event */
        mouse_event_t ev;
        ev.dx = dx;
        ev.dy = dy;
        ev.buttons = current_buttons;
        ev._pad[0] = ev._pad[1] = ev._pad[2] = 0;
        eq_push(&ev);
        break;
    }
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void mouse_init(void) {
    /* Enable the auxiliary mouse device on the i8042 controller */

    /* 1. Enable the auxiliary port */
    i8042_send_cmd(0xA8);

    /* 2. Read the controller configuration byte */
    i8042_send_cmd(0x20);
    uint8_t config = mouse_read();

    /* 3. Enable IRQ12 (bit 1) and make sure auxiliary clock is enabled (bit 5 clear) */
    config |= 0x02;      /* Enable aux IRQ (IRQ12) */
    config &= ~0x20;     /* Enable aux clock */

    /* 4. Write back the configuration */
    i8042_send_cmd(0x60);
    i8042_wait_write();
    outb(I8042_DATA, config);

    /* 5. Tell the mouse to use default settings */
    mouse_write(0xF6);
    mouse_read();  /* ACK */

    /* 6. Enable data reporting (mouse starts sending packets) */
    mouse_write(0xF4);
    mouse_read();  /* ACK */

    /* 7. Register IRQ12 handler (vector 44) and unmask IRQ12 */
    isr_register_handler(44, mouse_irq_handler);
    pic_clear_mask(12);

    /* Also ensure IRQ2 (cascade) is unmasked for slave PIC */
    pic_clear_mask(2);

    /* Start cursor at center of screen */
    mouse_x = screen_w / 2;
    mouse_y = screen_h / 2;
}

int mouse_poll_event(mouse_event_t *out) {
    return eq_pop(out);
}
