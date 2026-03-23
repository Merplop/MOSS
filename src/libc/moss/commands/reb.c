#include<stdint.h>
#include <sys/io.h>
#include<moss/commands.h>

void reb_cmd(void) {
	uint8_t g = 0x02;
	while (g & 0x02) {
		g = inb(0x64);
	}
	outb(0x64, 0xFE);
	__asm__ __volatile__("hlt");
}