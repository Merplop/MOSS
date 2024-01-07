#include<stdbool.h>
#include<stdint.h>

#define KEY_INFO_ADDRESS 0x1600;

#pragma once


uint8_t get_key(void) {
	const uint8_t *scancode_to_ascii = "\x00\x1B" "1234567890-=" "\x08"
	"\x00" "qwertyuiop[]" "\x0D\x1D" "asdfghjkl;'`" "\x00" "\\" 
	"zxcvbnm,./" "\x00\x00\x00" " ";
	uint8_t *num_row_shifts = ")!@#$%^&*(";
	uint8_t left_shift = 0;
	uint8_t left_ctrl = 0;
	uint8_t scancode = 0;
	uint8_t input_char = 0;
	uint8_t test_byte = 0;
	uint8_t oldKey = 0;

	while(1) {
		oldKey = scancode;
		while(1) {
			__asm__ __volatile__ ("inb $0x64, %%al" : "=a"(test_byte) );
			if (test_byte & 1) break;
		}
		__asm__ __volatile__ ("inb $0x60, %%al" : "=a"(scancode) );

		if (scancode == oldKey) {
			continue;
		} else if (scancode == 0x2A) {
			continue;
		} else if (scancode == 0xAA) {
			left_shift = 0;
		} else if (scancode == 0x1D) {
			left_ctrl = 1;
		} else if (scancode == 0x9D) {
			left_ctrl = 0;
			continue;
		} else if (scancode & (1 << 7)) {
			continue;
		} else if (scancode > 0x53) {
			continue;
		}
		input_char = scancode_to_ascii[scancode];

		if (left_shift != 1) {
			break;
		}

		if (input_char >= 'a' && input_char <= 'z') {
			input_char -= 0x20;
			break;
		}
		if (input_char >= '0' && input_char <= '9') {
			input_char -= 0x30;
			input_char = num_row_shifts[input_char];
			break;
		}
		if (input_char == '=')
			input_char = '+';
		else if (input_char == '/')
			input_char = '?';

		break;
	}
	*(uint8_t *)0x1600 = input_char;
	*(uint8_t *)0x1601 = scancode;
	*(uint8_t *)0x1602 = left_shift;
	*(uint8_t *)0x1603 = left_ctrl;

	left_shift = 0;
	left_ctrl = 0;

	return input_char;
}
