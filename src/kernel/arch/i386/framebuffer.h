#ifndef ARCH_I386_FRAMEBUFFER_H
#define ARCH_I386_FRAMEBUFFER_H

#include <stdint.h>
#include <stddef.h>

typedef struct {
    uint8_t  *address;
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;
    uint8_t   bpp;
    uint8_t   red_pos, red_size;
    uint8_t   green_pos, green_size;
    uint8_t   blue_pos, blue_size;
} framebuffer_info_t;

void framebuffer_init(framebuffer_info_t *info);
void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color);
void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h, uint32_t color);
void fb_clear(uint32_t color);
uint32_t fb_make_color(uint8_t r, uint8_t g, uint8_t b);
void fb_draw_char(unsigned char c, uint32_t x, uint32_t y, uint32_t fg, uint32_t bg);
void fb_scroll(uint32_t rows_pixels);
void fb_flush(void);
void fb_flush_full(void);
framebuffer_info_t *fb_get_info(void);
uint8_t *fb_get_shadow(void);

#endif
