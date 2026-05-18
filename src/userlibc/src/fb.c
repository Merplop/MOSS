/*
 * MOSS user-space libc — framebuffer mapping.
 */
#include <moss/fb.h>
#include <syscall.h>

uint32_t moss_fb_map(moss_fbinfo_t *info) {
    return (uint32_t)_syscall1(SYS_FBMAP, (uint32_t)info);
}

void moss_fb_flush(void) {
    _syscall0(SYS_FB_FLUSH);
}
