#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/io.h>
#include <kernel/tty.h>

#include "vga.h"

static const size_t VGA_WIDTH = 80;
static const size_t VGA_HEIGHT = 25;
static uint16_t* const VGA_MEMORY = (uint16_t*) 0xB8000;
static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_fg = VGA_COLOUR_LIGHT_GREY;
static uint8_t terminal_bg = VGA_COLOUR_BLACK;
static uint8_t terminal_colour;
static uint16_t* terminal_buffer;

void terminal_scroll(void) {
	const size_t buffer_size = (VGA_HEIGHT-1)*VGA_WIDTH;
	memmove(terminal_buffer, terminal_buffer+VGA_WIDTH, buffer_size*2);
	const size_t last_row_offset = VGA_HEIGHT-1;
	for (size_t x = 0; x < VGA_WIDTH; x++) {
		terminal_putentryat(' ', terminal_colour, x, last_row_offset);
	}
}

void change_colour_current(enum vga_colour fg, enum vga_colour bg) {
	terminal_fg = fg;
	terminal_bg = bg;
	terminal_colour = vga_entry_colour(fg, bg);
}

void change_colour(enum vga_colour fg, enum vga_colour bg) {
	terminal_fg = fg;
	terminal_bg = bg;
	terminal_colour = vga_entry_colour(fg, bg);
	for (size_t y = 0; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			terminal_putentryat(' ', terminal_colour, x, y);
		} 
	}
}


void enable_cursor(uint8_t cursor_start, uint8_t cursor_end) {
	outb(0x3D4, 0x0A);
	outb(0x3D5, (inb(0x3D5) & 0xC0) | cursor_start);
	outb(0x3D4, 0x0B);
	outb(0x3D5, (inb(0x3D5) & 0xE0) | cursor_end);
}

void disable_cursor() {
	outb(0x3D4, 0x0A);
	outb(0x3D5, 0x20);
}

void update_cursor(int x, int y) {
	uint16_t pos = y*VGA_WIDTH+x;
	outb(0x3D4, 0x0F);
	outb(0x3D5, (uint8_t)(pos & 0xFF));
	outb(0x3D4, 0x0E);
	outb(0x3D5, (uint8_t)((pos>>8)&0xFF));
}

void terminal_initialize(void) {
	terminal_row = 0;
	terminal_column = 0;
	terminal_buffer = VGA_MEMORY;
	terminal_colour = vga_entry_colour(terminal_fg, terminal_bg);
	for (size_t y = 0; y < VGA_HEIGHT; y++) {
		for (size_t x = 0; x < VGA_WIDTH; x++) {
			const size_t index = y * VGA_WIDTH + x;
			terminal_buffer[index] = vga_entry(' ', terminal_colour);
		}
	}
	enable_cursor(0, 0);
}

void terminal_setcolour(uint8_t colour) {
	terminal_colour = colour;
}

void terminal_putentryat(unsigned char c, uint8_t colour, size_t x, size_t y) {
	const size_t index = y * VGA_WIDTH + x;
	terminal_buffer[index] = vga_entry(c, colour);
}

void terminal_putchar(char c) {
	unsigned char uc = c;
	if (c == '\n') {
		if (terminal_row+1 == VGA_HEIGHT) {
			terminal_scroll();
			terminal_column = 0;
		} else {
			terminal_row++;
			terminal_column = 0;
		}
	} else {
		terminal_putentryat(uc, terminal_colour, terminal_column, terminal_row);
		if (++terminal_column == VGA_WIDTH) {
			terminal_column = 0;
			if (++terminal_row == VGA_HEIGHT)
				terminal_scroll();
		}
	} 
}

void terminal_write(const char* data, size_t size) {
	for (size_t i = 0; i < size && data[i] != '\0'; i++) {
		if (data[i] == '\r') {
			terminal_column = 0;
			update_cursor(terminal_column, terminal_row+1);
			continue;
		} else if (data[i] == '\b') {
			terminal_column--;
			terminal_putchar(' ');
			terminal_column--;
			update_cursor(terminal_column, terminal_row+1);
			continue;
		} else if (data[i] == 0x09) {
			terminal_column+=5;
			for (int i=0;i<5;i++) {
				terminal_putchar(' ');
			}
			continue;
		}
		terminal_putchar(data[i]);
		update_cursor(terminal_column, terminal_row+1);
	}
		
}

void terminal_writestring(const char* data) {
	terminal_write(data, strlen(data));
}
