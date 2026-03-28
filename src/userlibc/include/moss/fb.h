/* <moss/fb.h> — Framebuffer access for MOSS user-space programs */
#ifndef _MOSS_FB_H
#define _MOSS_FB_H

#include <stdint.h>

typedef struct {
    uint32_t address;    /* virtual address of the framebuffer */
    uint32_t width;
    uint32_t height;
    uint32_t pitch;      /* bytes per scanline */
    uint8_t  bpp;        /* bits per pixel (24 or 32) */
    uint8_t  red_pos, red_size;
    uint8_t  green_pos, green_size;
    uint8_t  blue_pos, blue_size;
} __attribute__((packed)) moss_fbinfo_t;

/* Map the framebuffer and fill info. Returns framebuffer address or 0. */
uint32_t moss_fb_map(moss_fbinfo_t *info);

#endif /* _MOSS_FB_H */
