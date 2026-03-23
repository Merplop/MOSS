#include<sys/io.h>

void set_pit(const uint8_t channel, const uint8_t mode, const uint16_t frequency) {
	if (channel > 2) return;

//	__asm__ __volatile__ ("cli");

	outb(0x43, (channel << 6) | (0x3 << 4) | (mode << 1));

	outb(0x40 + channel, (uint8_t)frequency);
	outb(0x40 + channel, (uint8_t)(frequency >> 8));

//	__asm__ __volatile__ ("sti");
}

void sleep_seconds(const uint16_t s) {
	__asm__ __volatile__ ("int $0x80" : : "a"(2), "b"(s*1000));
}

void sleep_milliseconds(const uint32_t m) {
	__asm__ __volatile__ ("int $0x80" : : "a"(2), "b"(m));
}
