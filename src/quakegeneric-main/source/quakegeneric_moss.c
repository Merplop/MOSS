/*
 * quakegeneric_moss.c — MOSS OS platform implementation for quakegeneric.
 *
 * Implements the QG_* callbacks using MOSS framebuffer and keyboard syscalls.
 * Modeled after doomgeneric_moss.c.
 */

#include "quakegeneric.h"
#include "quakekeys.h"
#include <moss/fb.h>
#include <moss/keyboard.h>
#include <unistd.h>
#include <string.h>
#include <stdint.h>

static moss_fbinfo_t fb;
static uint32_t *fb_addr;
static uint32_t rgbpixels[QUAKEGENERIC_RES_X * QUAKEGENERIC_RES_Y];
static unsigned char pal[768];

/* ------------------------------------------------------------------ */
/*  Key buffer (circular)                                              */
/* ------------------------------------------------------------------ */

#define KEYBUFFERSIZE 32
static int keybuffer[KEYBUFFERSIZE];
static int keybuffer_len;
static int keybuffer_start;

static void KeyPush(int down, int key)
{
    if (keybuffer_len >= KEYBUFFERSIZE)
        return;
    if (down)
        key = -key;
    keybuffer[(keybuffer_start + keybuffer_len) % KEYBUFFERSIZE] = key;
    keybuffer_len++;
}

/* ------------------------------------------------------------------ */
/*  PS/2 scancode → Quake key mapping                                  */
/* ------------------------------------------------------------------ */

static int scancode_to_quakekey(uint8_t sc, uint8_t extended)
{
    if (extended) {
        switch (sc) {
            case 0x48: return K_UPARROW;
            case 0x50: return K_DOWNARROW;
            case 0x4B: return K_LEFTARROW;
            case 0x4D: return K_RIGHTARROW;
            case 0x47: return K_HOME;
            case 0x4F: return K_END;
            case 0x49: return K_PGUP;
            case 0x51: return K_PGDN;
            case 0x52: return K_INS;
            case 0x53: return K_DEL;
            default:   return 0;
        }
    }
    switch (sc) {
        case 0x01: return K_ESCAPE;
        case 0x1C: return K_ENTER;
        case 0x0F: return K_TAB;
        case 0x39: return K_SPACE;
        case 0x0E: return K_BACKSPACE;
        case 0x1D: return K_CTRL;
        case 0x38: return K_ALT;
        case 0x2A: case 0x36: return K_SHIFT;
        /* F-keys */
        case 0x3B: return K_F1;  case 0x3C: return K_F2;
        case 0x3D: return K_F3;  case 0x3E: return K_F4;
        case 0x3F: return K_F5;  case 0x40: return K_F6;
        case 0x41: return K_F7;  case 0x42: return K_F8;
        case 0x43: return K_F9;  case 0x44: return K_F10;
        case 0x57: return K_F11; case 0x58: return K_F12;
        case 0xC5: return K_PAUSE;
        /* Number row */
        case 0x02: return '1'; case 0x03: return '2';
        case 0x04: return '3'; case 0x05: return '4';
        case 0x06: return '5'; case 0x07: return '6';
        case 0x08: return '7'; case 0x09: return '8';
        case 0x0A: return '9'; case 0x0B: return '0';
        case 0x0C: return '-'; case 0x0D: return '=';
        case 0x1A: return '['; case 0x1B: return ']';
        case 0x27: return ';'; case 0x28: return '\'';
        case 0x29: return '`'; case 0x2B: return '\\';
        case 0x33: return ','; case 0x34: return '.';
        case 0x35: return '/';
        /* Letter keys — Quake expects lowercase ASCII */
        case 0x1E: return 'a'; case 0x30: return 'b';
        case 0x2E: return 'c'; case 0x20: return 'd';
        case 0x12: return 'e'; case 0x21: return 'f';
        case 0x22: return 'g'; case 0x23: return 'h';
        case 0x17: return 'i'; case 0x24: return 'j';
        case 0x25: return 'k'; case 0x26: return 'l';
        case 0x32: return 'm'; case 0x31: return 'n';
        case 0x18: return 'o'; case 0x19: return 'p';
        case 0x10: return 'q'; case 0x13: return 'r';
        case 0x1F: return 's'; case 0x14: return 't';
        case 0x16: return 'u'; case 0x2F: return 'v';
        case 0x11: return 'w'; case 0x2D: return 'x';
        case 0x15: return 'y'; case 0x2C: return 'z';
        default: return 0;
    }
}

/* Drain MOSS keyboard events into the key buffer */
static void PumpKeyEvents(void)
{
    key_event_t ev;
    while (moss_poll_key(&ev)) {
        int qk = scancode_to_quakekey(ev.scancode,
                     ev.flags & KEY_EVENT_EXTENDED);
        if (qk != 0)
            KeyPush((ev.flags & KEY_EVENT_PRESS) ? 1 : 0, qk);
    }
}

/* ------------------------------------------------------------------ */
/*  QG callbacks                                                       */
/* ------------------------------------------------------------------ */

void QG_Init(void)
{
    fb_addr = (uint32_t *)(uintptr_t)moss_fb_map(&fb);
    keybuffer_len = 0;
    keybuffer_start = 0;
}

int QG_GetKey(int *down, int *key)
{
    PumpKeyEvents();

    if (keybuffer_len == 0)
        return 0;

    *key = keybuffer[keybuffer_start];
    *down = (*key < 0) ? 1 : 0;
    if (*key < 0)
        *key = -*key;
    keybuffer_start = (keybuffer_start + 1) % KEYBUFFERSIZE;
    keybuffer_len--;
    return 1;
}

void QG_GetMouseMove(int *x, int *y)
{
    *x = 0;
    *y = 0;
}

void QG_GetJoyAxes(float *axes)
{
    int i;
    for (i = 0; i < QUAKEGENERIC_JOY_MAX_AXES; i++)
        axes[i] = 0.0f;
}

void QG_Quit(void)
{
    _exit(0);
}

void QG_SetPalette(unsigned char palette[768])
{
    memcpy(pal, palette, 768);
}

void QG_DrawFrame(void *pixels)
{
    if (!fb_addr)
        return;

    /* Convert 8-bit indexed pixels to 32-bit ARGB using the palette */
    const uint8_t *src = (const uint8_t *)pixels;
    int total = QUAKEGENERIC_RES_X * QUAKEGENERIC_RES_Y;
    int i;
    for (i = 0; i < total; i++) {
        const uint8_t *entry = &pal[src[i] * 3];
        rgbpixels[i] = (0xFFu << 24) | ((uint32_t)entry[0] << 16)
                      | ((uint32_t)entry[1] << 8) | (uint32_t)entry[2];
    }

    /* Scale to framebuffer */
    uint32_t x_scale = fb.width / QUAKEGENERIC_RES_X;
    uint32_t y_scale = fb.height / QUAKEGENERIC_RES_Y;
    uint32_t scale = x_scale < y_scale ? x_scale : y_scale;
    if (scale == 0) scale = 1;
    uint32_t scaled_w = QUAKEGENERIC_RES_X * scale;
    uint32_t row_bytes = scaled_w * sizeof(uint32_t);

    for (uint32_t y = 0; y < QUAKEGENERIC_RES_Y; y++) {
        uint32_t *first_row = (uint32_t *)((uint8_t *)fb_addr +
            (y * scale) * fb.pitch);
        const uint32_t *row_src = &rgbpixels[y * QUAKEGENERIC_RES_X];
        uint32_t x;
        for (x = 0; x < QUAKEGENERIC_RES_X; x++) {
            uint32_t pixel = row_src[x];
            uint32_t dst_x = x * scale;
            uint32_t sx;
            for (sx = 0; sx < scale; sx++)
                first_row[dst_x + sx] = pixel;
        }
        uint32_t sy;
        for (sy = 1; sy < scale; sy++) {
            uint32_t *dup_row = (uint32_t *)((uint8_t *)fb_addr +
                (y * scale + sy) * fb.pitch);
            memcpy(dup_row, first_row, row_bytes);
        }
    }
    moss_fb_flush();
}

/* ------------------------------------------------------------------ */
/*  main                                                               */
/* ------------------------------------------------------------------ */

int main(int argc, char **argv)
{
    double oldtime, newtime;

    QG_Create(argc, argv);

    oldtime = (double)getticks() / 1000.0;

    while (1) {
        newtime = (double)getticks() / 1000.0;
        QG_Tick(newtime - oldtime);
        oldtime = newtime;
    }

    return 0;
}
