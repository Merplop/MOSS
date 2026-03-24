#include <stdint.h>
#include <kernel/keyboard.h>

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

uint8_t get_key(void) {
    static uint8_t shift = 0;
    static uint8_t ctrl = 0;
    static uint8_t caps_lock = 0;
    static uint8_t extended = 0;
    uint8_t scancode;
    uint8_t input_char;
    uint8_t status;

    while (1) {
        do {
            __asm__ __volatile__("inb $0x64, %%al" : "=a"(status));
        } while (!(status & 1));

        __asm__ __volatile__("inb $0x60, %%al" : "=a"(scancode));

        /* Extended scancode prefix (arrow keys, right Ctrl, etc.) */
        if (scancode == 0xE0) {
            extended = 1;
            continue;
        }
        if (extended) {
            extended = 0;
            if (scancode == 0x1D) { ctrl = 1; continue; }  /* Right Ctrl make */
            if (scancode == 0x9D) { ctrl = 0; continue; }  /* Right Ctrl break */
            continue;
        }

        /* Modifier key press/release */
        switch (scancode) {
        case LSHIFT_MAKE:  case RSHIFT_MAKE:  shift = 1; continue;
        case LSHIFT_BREAK: case RSHIFT_BREAK: shift = 0; continue;
        case LCTRL_MAKE:  ctrl = 1; continue;
        case LCTRL_BREAK: ctrl = 0; continue;
        case CAPS_LOCK_MAKE: caps_lock ^= 1; continue;
        }

        /* Ignore all other break (key release) codes */
        if (scancode & 0x80)
            continue;

        /* Look up ASCII value based on shift state */
        input_char = shift
            ? shifted_scancode_to_ascii[scancode]
            : scancode_to_ascii[scancode];

        /* Caps Lock inverts letter case */
        if (caps_lock) {
            if (input_char >= 'a' && input_char <= 'z')
                input_char -= 0x20;
            else if (input_char >= 'A' && input_char <= 'Z')
                input_char += 0x20;
        }

        /* Ctrl+letter produces control codes (Ctrl+A=1 .. Ctrl+Z=26) */
        if (ctrl) {
            uint8_t lower = input_char | 0x20;
            if (lower >= 'a' && lower <= 'z') {
                input_char = lower - 'a' + 1;
                break;
            }
        }

        /* Skip non-printable keys (F-keys, Alt, Num Lock, etc.) */
        if (input_char == 0)
            continue;

        break;
    }

    *(uint8_t *)0x1600 = input_char;
    *(uint8_t *)0x1601 = scancode;
    *(uint8_t *)0x1602 = shift;
    *(uint8_t *)0x1603 = ctrl;

    return input_char;
}
