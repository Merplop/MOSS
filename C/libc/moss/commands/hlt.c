void hlt_cmd(void) {
	printf("Halting the CPU - you will have to hard reboot your PC");
	__asm__ __volatile__("hlt");
}