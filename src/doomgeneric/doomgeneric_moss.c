#include "doomgeneric.h"
#include "doomkeys.h"
#include <moss/fb.h>
#include <moss/keyboard.h>
#include <unistd.h>
#include <syscall.h>
#include <string.h>
#include <stdint.h>

static moss_fbinfo_t fb;
static uint32_t *fb_addr;

// Map Doom keys to PS/2 scancodes
static unsigned char scancode_to_doomkey(uint8_t sc, uint8_t extended) {
    switch (sc) {
        case 0x48: return extended ? KEY_UPARROW    : 0;
        case 0x50: return extended ? KEY_DOWNARROW   : 0;
        case 0x4B: return extended ? KEY_LEFTARROW   : 0;
        case 0x4D: return extended ? KEY_RIGHTARROW  : 0;
        case 0x1D: return KEY_FIRE;        // Ctrl
        case 0x39: return KEY_USE;         // Space
        case 0x01: return KEY_ESCAPE;
        case 0x1C: return KEY_ENTER;
        case 0x2A: case 0x36: return KEY_RSHIFT;
        case 0x0F: return KEY_TAB;
        // number keys for weapon select
        case 0x02: return '1'; case 0x03: return '2';
        case 0x04: return '3'; case 0x05: return '4';
        case 0x06: return '5'; case 0x07: return '6';
        case 0x08: return '7';
        default: return 0;
    }
}

void DG_Init(void) {
    fb_addr = (uint32_t *)(uintptr_t)moss_fb_map(&fb);
}

void DG_DrawFrame(void) {
    // doomgeneric renders into DG_ScreenBuffer (320x200 ARGB)
    // Scale/copy to framebuffer
    uint32_t x_scale = fb.width / DOOMGENERIC_RESX;
    uint32_t y_scale = fb.height / DOOMGENERIC_RESY;
    uint32_t scale = x_scale < y_scale ? x_scale : y_scale;

    for (uint32_t y = 0; y < DOOMGENERIC_RESY; y++) {
        for (uint32_t x = 0; x < DOOMGENERIC_RESX; x++) {
            uint32_t pixel = DG_ScreenBuffer[y * DOOMGENERIC_RESX + x];
            for (uint32_t sy = 0; sy < scale; sy++) {
                uint32_t *row = (uint32_t *)((uint8_t *)fb_addr +
                    (y * scale + sy) * fb.pitch);
                for (uint32_t sx = 0; sx < scale; sx++) {
                    row[x * scale + sx] = pixel;
                }
            }
        }
    }
}

void DG_SleepMs(uint32_t ms) {
    usleep(ms * 1000);
}

uint32_t DG_GetTicksMs(void) {
    return getticks() * 10;  // 100 Hz ticks → milliseconds
}

int DG_GetKey(int *pressed, unsigned char *doomKey) {
    key_event_t ev;
    if (!moss_poll_key(&ev))
        return 0;
    *pressed = (ev.flags & KEY_EVENT_PRESS) ? 1 : 0;
    unsigned char dk = scancode_to_doomkey(ev.scancode,
                           ev.flags & KEY_EVENT_EXTENDED);
    if (dk == 0) return 0;
    *doomKey = dk;
    return 1;
}

void DG_SetWindowTitle(const char *title) {
    (void)title;
}

int main(int argc, char **argv) {
    doomgeneric_Create(argc, argv);
    while (1)
        doomgeneric_Tick();
    return 0;
}