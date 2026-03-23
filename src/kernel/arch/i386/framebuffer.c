#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "framebuffer.h"
#include "font8x16.h"

static framebuffer_info_t fb;

void framebuffer_init(framebuffer_info_t *info) {
    fb = *info;
}

framebuffer_info_t *fb_get_info(void) {
    return &fb;
}

uint32_t fb_make_color(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << fb.red_pos) |
           ((uint32_t)g << fb.green_pos) |
           ((uint32_t)b << fb.blue_pos);
}

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (x >= fb.width || y >= fb.height)
        return;
    uint32_t offset = y * fb.pitch + x * (fb.bpp / 8);
    uint8_t *pixel = fb.address + offset;
    if (fb.bpp == 32) {
        *((uint32_t *)pixel) = color;
    } else if (fb.bpp == 24) {
        pixel[0] = color & 0xFF;
        pixel[1] = (color >> 8) & 0xFF;
        pixel[2] = (color >> 16) & 0xFF;
    }
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (fb.bpp == 32) {
        for (uint32_t row = y; row < y + h && row < fb.height; row++) {
            uint32_t *line = (uint32_t *)(fb.address + row * fb.pitch);
            uint32_t end = x + w;
            if (end > fb.width) end = fb.width;
            for (uint32_t col = x; col < end; col++) {
                line[col] = color;
            }
        }
    } else {
        for (uint32_t row = y; row < y + h && row < fb.height; row++) {
            for (uint32_t col = x; col < x + w && col < fb.width; col++) {
                fb_put_pixel(col, row, color);
            }
        }
    }
}

void fb_clear(uint32_t color) {
    if (fb.bpp == 32) {
        for (uint32_t row = 0; row < fb.height; row++) {
            uint32_t *line = (uint32_t *)(fb.address + row * fb.pitch);
            for (uint32_t col = 0; col < fb.width; col++) {
                line[col] = color;
            }
        }
    } else {
        fb_fill_rect(0, 0, fb.width, fb.height, color);
    }
}

void fb_draw_char(unsigned char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font_8x16[c];

    if (fb.bpp == 32) {
        for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
            uint32_t py = y + row;
            if (py >= fb.height) break;
            uint32_t *line = (uint32_t *)(fb.address + py * fb.pitch);
            uint8_t bits = glyph[row];
            for (uint32_t col = 0; col < FONT_WIDTH; col++) {
                uint32_t px = x + col;
                if (px >= fb.width) break;
                line[px] = (bits & (0x80 >> col)) ? fg : bg;
            }
        }
    } else {
        for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
            uint8_t bits = glyph[row];
            for (uint32_t col = 0; col < FONT_WIDTH; col++) {
                fb_put_pixel(x + col, y + row,
                             (bits & (0x80 >> col)) ? fg : bg);
            }
        }
    }
}

void fb_scroll(uint32_t rows_pixels) {
    uint32_t copy_bytes = (fb.height - rows_pixels) * fb.pitch;
    memmove(fb.address, fb.address + rows_pixels * fb.pitch, copy_bytes);
}
