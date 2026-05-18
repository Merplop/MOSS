#ifndef        _SYS_IO_H
#define        _SYS_IO_H        1

#include <stdint.h>

static inline void outb(uint16_t port, uint8_t val) {
    asm volatile ( "outb %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint8_t inb(uint16_t port) {
    uint8_t ret;
    asm volatile ( "inb %1, %0"
                   : "=a"(ret)
                   : "Nd"(port)
                   : "memory");
    return ret;
}

static inline void outw(uint16_t port, uint16_t val) {
    asm volatile ( "outw %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint16_t inw(uint16_t port) {
    uint16_t ret;
    asm volatile ( "inw %1, %0"
                   : "=a"(ret)
                   : "Nd"(port)
                   : "memory");
    return ret;
}

static inline void insw(uint16_t port, void *addr, uint32_t count) {
    asm volatile ("rep insw"
                  : "+D"(addr), "+c"(count)
                  : "d"(port)
                  : "memory");
}

static inline void outsw(uint16_t port, const void *addr, uint32_t count) {
    asm volatile ("rep outsw"
                  : "+S"(addr), "+c"(count)
                  : "d"(port)
                  : "memory");
}

static inline void outl(uint16_t port, uint32_t val) {
    asm volatile ( "outl %0, %1" : : "a"(val), "Nd"(port) : "memory");
}

static inline uint32_t inl(uint16_t port) {
    uint32_t ret;
    asm volatile ( "inl %1, %0"
                   : "=a"(ret)
                   : "Nd"(port)
                   : "memory");
    return ret;
}

static inline void io_wait(void) {
    outb(0x80, 0);
}

#endif /* _SYS_IO_H */
