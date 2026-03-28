/* hello.c — user program for MOSS */
void _start(void) {
    const char msg[] = "Hello from user mode!\r\n";
    /* SYS_WRITE(fd=1, buf, len) */
    asm volatile("int $0x80" :: "a"(1), "b"(1), "c"(msg), "d"(23));
    /* SYS_EXIT(0) */
    asm volatile("int $0x80" :: "a"(0), "b"(0));
}
