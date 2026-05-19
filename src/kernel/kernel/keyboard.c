/*
 * PS/2 Keyboard driver — IRQ1-driven with event queue.
 * MOSS Kernel
 *
 * Maintains:
 *   - A ring buffer of key_event_t for press/release events
 *   - A 128-byte key-state bitmap (which keys are currently held)
 *   - Current modifier state (shift, ctrl, alt, capslock)
 *
 * The shell's blocking get_key() consumes from the same queue.
 * User-space programs use SYS_POLL_KEY for non-blocking access.
 */

#include <stdint.h>
#include <kernel/keyboard.h>
#include <kernel/sched.h>

/* Forward declarations for arch-specific functions */
struct isr_regs;
typedef void (*isr_handler_t)(struct isr_regs *regs);
void isr_register_handler(uint8_t n, isr_handler_t handler);
void pic_clear_mask(uint8_t irq);

/* ------------------------------------------------------------------ */
/*  Scancode-to-ASCII translation tables                               */
/* ------------------------------------------------------------------ */

static const uint8_t scancode_to_ascii[128] = {
    [0x01] = 0x1B,
    [0x02] = '1',  [0x03] = '2',  [0x04] = '3',  [0x05] = '4',
    [0x06] = '5',  [0x07] = '6',  [0x08] = '7',  [0x09] = '8',
    [0x0A] = '9',  [0x0B] = '0',  [0x0C] = '-',  [0x0D] = '=',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'q',  [0x11] = 'w',  [0x12] = 'e',  [0x13] = 'r',
    [0x14] = 't',  [0x15] = 'y',  [0x16] = 'u',  [0x17] = 'i',
    [0x18] = 'o',  [0x19] = 'p',  [0x1A] = '[',  [0x1B] = ']',
    [0x1C] = '\r',
    [0x1E] = 'a',  [0x1F] = 's',  [0x20] = 'd',  [0x21] = 'f',
    [0x22] = 'g',  [0x23] = 'h',  [0x24] = 'j',  [0x25] = 'k',
    [0x26] = 'l',  [0x27] = ';',  [0x28] = '\'', [0x29] = '`',
    [0x2B] = '\\',
    [0x2C] = 'z',  [0x2D] = 'x',  [0x2E] = 'c',  [0x2F] = 'v',
    [0x30] = 'b',  [0x31] = 'n',  [0x32] = 'm',  [0x33] = ',',
    [0x34] = '.',  [0x35] = '/',
    [0x37] = '*',
    [0x39] = ' ',
    [0x47] = '7',  [0x48] = '8',  [0x49] = '9',  [0x4A] = '-',
    [0x4B] = '4',  [0x4C] = '5',  [0x4D] = '6',  [0x4E] = '+',
    [0x4F] = '1',  [0x50] = '2',  [0x51] = '3',  [0x52] = '0',
    [0x53] = '.',
};

static const uint8_t shifted_scancode_to_ascii[128] = {
    [0x01] = 0x1B,
    [0x02] = '!',  [0x03] = '@',  [0x04] = '#',  [0x05] = '$',
    [0x06] = '%',  [0x07] = '^',  [0x08] = '&',  [0x09] = '*',
    [0x0A] = '(',  [0x0B] = ')',  [0x0C] = '_',  [0x0D] = '+',
    [0x0E] = '\b', [0x0F] = '\t',
    [0x10] = 'Q',  [0x11] = 'W',  [0x12] = 'E',  [0x13] = 'R',
    [0x14] = 'T',  [0x15] = 'Y',  [0x16] = 'U',  [0x17] = 'I',
    [0x18] = 'O',  [0x19] = 'P',  [0x1A] = '{',  [0x1B] = '}',
    [0x1C] = '\r',
    [0x1E] = 'A',  [0x1F] = 'S',  [0x20] = 'D',  [0x21] = 'F',
    [0x22] = 'G',  [0x23] = 'H',  [0x24] = 'J',  [0x25] = 'K',
    [0x26] = 'L',  [0x27] = ':',  [0x28] = '"',  [0x29] = '~',
    [0x2B] = '|',
    [0x2C] = 'Z',  [0x2D] = 'X',  [0x2E] = 'C',  [0x2F] = 'V',
    [0x30] = 'B',  [0x31] = 'N',  [0x32] = 'M',  [0x33] = '<',
    [0x34] = '>',  [0x35] = '?',
    [0x37] = '*',
    [0x39] = ' ',
    [0x47] = '7',  [0x48] = '8',  [0x49] = '9',  [0x4A] = '-',
    [0x4B] = '4',  [0x4C] = '5',  [0x4D] = '6',  [0x4E] = '+',
    [0x4F] = '1',  [0x50] = '2',  [0x51] = '3',  [0x52] = '0',
    [0x53] = '.',
};

/* ------------------------------------------------------------------ */
/*  Event ring buffer                                                  */
/* ------------------------------------------------------------------ */

#define EVENT_QUEUE_SIZE 64   /* must be power of 2 */

static key_event_t event_queue[EVENT_QUEUE_SIZE];
static volatile uint32_t eq_head = 0;  /* next write position */
static volatile uint32_t eq_tail = 0;  /* next read position */

static void eq_push(const key_event_t *ev) {
    uint32_t next = (eq_head + 1) & (EVENT_QUEUE_SIZE - 1);
    if (next == eq_tail)
        return;  /* queue full, drop event */
    event_queue[eq_head] = *ev;
    eq_head = next;
}

static int eq_pop(key_event_t *out) {
    if (eq_tail == eq_head)
        return 0;  /* empty */
    *out = event_queue[eq_tail];
    eq_tail = (eq_tail + 1) & (EVENT_QUEUE_SIZE - 1);
    return 1;
}

/* ------------------------------------------------------------------ */
/*  Key state bitmap (256 entries: 128 normal + 128 extended)          */
/* ------------------------------------------------------------------ */

static uint8_t key_state[256 / 8];  /* 32 bytes, one bit per scancode */

static void set_key_state(uint8_t code, int pressed) {
    if (pressed)
        key_state[code / 8] |= (1 << (code % 8));
    else
        key_state[code / 8] &= ~(1 << (code % 8));
}

int keyboard_is_pressed(uint8_t code) {
    return (key_state[code / 8] >> (code % 8)) & 1;
}

/* ------------------------------------------------------------------ */
/*  Modifier tracking                                                  */
/* ------------------------------------------------------------------ */

static uint8_t modifiers = 0;

uint8_t keyboard_get_modifiers(void) {
    return modifiers;
}

/* ------------------------------------------------------------------ */
/*  IRQ1 handler                                                       */
/* ------------------------------------------------------------------ */

static uint8_t extended_flag = 0;

static void keyboard_irq_handler(struct isr_regs *regs) {
    (void)regs;

    uint8_t scancode;
    asm volatile("inb $0x60, %%al" : "=a"(scancode));

    /* Extended scancode prefix */
    if (scancode == 0xE0) {
        extended_flag = 1;
        return;
    }

    uint8_t is_extended = extended_flag;
    extended_flag = 0;

    /* Determine press/release */
    uint8_t is_release = (scancode & 0x80) != 0;
    uint8_t raw_sc = scancode & 0x7F;

    /* Compute a combined code: extended keys get +128 offset */
    uint8_t combined = is_extended ? (raw_sc | 0x80) : raw_sc;

    /* Update key state bitmap */
    set_key_state(combined, !is_release);

    /* Update modifier tracking */
    if (!is_extended) {
        switch (raw_sc) {
        case 0x2A: /* Left Shift */
        case 0x36: /* Right Shift */
            if (is_release)
                modifiers &= ~KEY_MOD_SHIFT;
            else
                modifiers |= KEY_MOD_SHIFT;
            break;
        case 0x1D: /* Left Ctrl */
            if (is_release)
                modifiers &= ~KEY_MOD_CTRL;
            else
                modifiers |= KEY_MOD_CTRL;
            break;
        case 0x38: /* Left Alt */
            if (is_release)
                modifiers &= ~KEY_MOD_ALT;
            else
                modifiers |= KEY_MOD_ALT;
            break;
        case 0x3A: /* Caps Lock (toggle on press only) */
            if (!is_release)
                modifiers ^= KEY_MOD_CAPSLOCK;
            break;
        }
    } else {
        /* Extended modifiers */
        switch (raw_sc) {
        case 0x1D: /* Right Ctrl */
            if (is_release)
                modifiers &= ~KEY_MOD_CTRL;
            else
                modifiers |= KEY_MOD_CTRL;
            break;
        case 0x38: /* Right Alt */
            if (is_release)
                modifiers &= ~KEY_MOD_ALT;
            else
                modifiers |= KEY_MOD_ALT;
            break;
        }
    }

    /* Build the event */
    key_event_t ev;
    ev.scancode  = raw_sc;
    ev.flags     = is_release ? KEY_EVENT_RELEASE : KEY_EVENT_PRESS;
    if (is_extended)
        ev.flags |= KEY_EVENT_EXTENDED;
    ev.modifiers = modifiers;

    /* Translate to ASCII (only for key presses of non-extended keys) */
    ev.ascii = 0;
    if (!is_release && !is_extended && raw_sc < 128) {
        uint8_t shift = (modifiers & KEY_MOD_SHIFT) != 0;
        uint8_t ch = shift
            ? shifted_scancode_to_ascii[raw_sc]
            : scancode_to_ascii[raw_sc];

        /* Apply Caps Lock to letters */
        if (modifiers & KEY_MOD_CAPSLOCK) {
            if (ch >= 'a' && ch <= 'z')
                ch -= 0x20;
            else if (ch >= 'A' && ch <= 'Z')
                ch += 0x20;
        }

        /* Ctrl+letter → control code */
        if ((modifiers & KEY_MOD_CTRL) && ch) {
            uint8_t lower = ch | 0x20;
            if (lower >= 'a' && lower <= 'z')
                ch = lower - 'a' + 1;
        }

        ev.ascii = ch;
    }

    /* Ctrl+C → send SIGINT to the foreground (current) task */
    if (ev.ascii == 3) {  /* ASCII ETX = Ctrl+C */
        task_t *fg = get_current_task();
        if (fg)
            task_send_signal(fg, SIGINT);
    }

    eq_push(&ev);
}

/* ------------------------------------------------------------------ */
/*  Public API                                                         */
/* ------------------------------------------------------------------ */

void keyboard_init(void) {
    /* Drain any pending scancodes */
    uint8_t status;
    asm volatile("inb $0x64, %%al" : "=a"(status));
    while (status & 1) {
        uint8_t dummy;
        asm volatile("inb $0x60, %%al" : "=a"(dummy));
        (void)dummy;
        asm volatile("inb $0x64, %%al" : "=a"(status));
    }

    /* Register IRQ1 handler (vector 33) and unmask IRQ1 */
    isr_register_handler(33, keyboard_irq_handler);
    pic_clear_mask(1);
}

int keyboard_poll_event(key_event_t *out) {
    return eq_pop(out);
}

int keyboard_has_events(void) {
    return eq_tail != eq_head;
}

/* Check if there is a key-press event with ASCII available.
 * Used by the TTY layer to properly report readability — release events
 * and non-ASCII events don't produce characters for read(). */
int keyboard_has_ascii_press(void) {
    uint32_t pos = eq_tail;
    while (pos != eq_head) {
        key_event_t *ev = &event_queue[pos];
        if ((ev->flags & KEY_EVENT_PRESS) && ev->ascii != 0)
            return 1;
        pos = (pos + 1) & (EVENT_QUEUE_SIZE - 1);
    }
    return 0;
}

/*
 * Blocking get_key() — backwards-compatible with the shell.
 * Waits for a key-press event with a non-zero ASCII character.
 */
uint8_t get_key(void) {
    key_event_t ev;
    while (1) {
        if (eq_pop(&ev)) {
            /* Only return on press events with a printable/control ASCII */
            if ((ev.flags & KEY_EVENT_PRESS) && ev.ascii != 0)
                return ev.ascii;
        } else {
            /* No events pending — yield CPU until next interrupt */
            asm volatile("sti; hlt");
        }
    }
}
