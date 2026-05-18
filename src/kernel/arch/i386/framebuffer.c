#include <stdint.h>
#include <stddef.h>
#include <string.h>

#include "framebuffer.h"
#include "font8x16.h"

static framebuffer_info_t fb;

/* Shadow framebuffer in RAM for fast rendering.
 * 1920×1080×4 = 8,294,400 bytes. We render here first, then flush to MMIO. */
#define SHADOW_FB_MAX (1920 * 1080 * 4)
static uint8_t shadow_fb[SHADOW_FB_MAX] __attribute__((aligned(4096)));

/* Dirty rectangle tracking */
static uint32_t dirty_y_min, dirty_y_max; /* pixel rows */
static uint32_t dirty_x_min, dirty_x_max; /* pixel columns */
static int dirty = 0;

static void mark_dirty(uint32_t x_start, uint32_t x_end, uint32_t y_start, uint32_t y_end) {
    if (!dirty) {
        dirty_x_min = x_start;
        dirty_x_max = x_end;
        dirty_y_min = y_start;
        dirty_y_max = y_end;
        dirty = 1;
    } else {
        if (x_start < dirty_x_min) dirty_x_min = x_start;
        if (x_end > dirty_x_max) dirty_x_max = x_end;
        if (y_start < dirty_y_min) dirty_y_min = y_start;
        if (y_end > dirty_y_max) dirty_y_max = y_end;
    }
}

void fb_flush(void) {
    if (!dirty) return;
    if (dirty_y_max > fb.height) dirty_y_max = fb.height;
    if (dirty_x_max > fb.width) dirty_x_max = fb.width;

    uint32_t bpp_bytes = fb.bpp / 8;
    uint32_t x_byte_start = dirty_x_min * bpp_bytes;
    uint32_t x_byte_end   = dirty_x_max * bpp_bytes;
    uint32_t row_bytes = x_byte_end - x_byte_start;

    for (uint32_t row = dirty_y_min; row < dirty_y_max; row++) {
        uint32_t offset = row * fb.pitch + x_byte_start;
        memcpy(fb.address + offset, shadow_fb + offset, row_bytes);
    }
    dirty = 0;
}

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
    uint8_t *pixel = shadow_fb + offset;
    if (fb.bpp == 32) {
        *((uint32_t *)pixel) = color;
    } else if (fb.bpp == 24) {
        pixel[0] = color & 0xFF;
        pixel[1] = (color >> 8) & 0xFF;
        pixel[2] = (color >> 16) & 0xFF;
    }
    mark_dirty(x, x + 1, y, y + 1);
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color) {
    if (fb.bpp == 32) {
        for (uint32_t row = y; row < y + h && row < fb.height; row++) {
            uint32_t *line = (uint32_t *)(shadow_fb + row * fb.pitch);
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
    uint32_t x_end = x + w;
    uint32_t y_end = y + h;
    if (x_end > fb.width) x_end = fb.width;
    if (y_end > fb.height) y_end = fb.height;
    mark_dirty(x, x_end, y, y_end);
}

void fb_clear(uint32_t color) {
    if (fb.bpp == 32) {
        for (uint32_t row = 0; row < fb.height; row++) {
            uint32_t *line = (uint32_t *)(shadow_fb + row * fb.pitch);
            for (uint32_t col = 0; col < fb.width; col++) {
                line[col] = color;
            }
        }
    } else {
        fb_fill_rect(0, 0, fb.width, fb.height, color);
    }
    mark_dirty(0, fb.width, 0, fb.height);
}

void fb_draw_char(unsigned char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font_8x16[c];

    if (fb.bpp == 32) {
        for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
            uint32_t py = y + row;
            if (py >= fb.height) break;
            uint32_t *line = (uint32_t *)(shadow_fb + py * fb.pitch);
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
    uint32_t x_end = x + FONT_WIDTH;
    uint32_t y_end = y + FONT_HEIGHT;
    if (x_end > fb.width) x_end = fb.width;
    if (y_end > fb.height) y_end = fb.height;
    mark_dirty(x, x_end, y, y_end);
}

void fb_scroll(uint32_t rows_pixels) {
    uint32_t copy_bytes = (fb.height - rows_pixels) * fb.pitch;
    memmove(shadow_fb, shadow_fb + rows_pixels * fb.pitch, copy_bytes);
    mark_dirty(0, fb.width, 0, fb.height);
}

uint8_t *fb_get_shadow(void) {
    return shadow_fb;
}

void fb_flush_full(void) {
    uint32_t fb_size = fb.pitch * fb.height;
    memcpy(fb.address, shadow_fb, fb_size);
    dirty = 0;
}
