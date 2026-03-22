// MOSS KERNEL - PC Speaker / Sound
// (C) Miro Haapalainen, 2024

#include <stdio.h>
#include <stdint.h>
#include <sys/io.h>
#include <kernel/keyboard.h>
#include <kernel/tty.h>
#include <kernel/kernel.h>

void play_sound(uint32_t nFrequence) {
	uint32_t Div;
	uint8_t tmp;

	Div = 1193180 / nFrequence;
	outb(0x43, 0xb6);
	outb(0x42, (uint8_t) (Div));
	outb(0x42, (uint8_t) (Div >> 8));

	tmp = inb(0x61);
	if (tmp != (tmp | 3)) {
		outb(0x61, tmp | 3);
	}
}

void enable_speaker(void) {
	uint8_t temp = inb(0x61);
	outb(0x61, temp | 3);
}

void disable_speaker(void) {
	uint8_t temp = inb(0x61);
	outb(0x61, temp & 0xFC);
}

void nosound(void) {
	uint8_t tmp = inb(0x61) & 0xFC;
	outb(0x61, tmp);
}

void beep_cmd(void) {
	if (argc != 2) {
		printf(ARG_COUNT_ERROR);
		return;
	}
	int freq = atoi(argv[1]);
	play_sound(freq);
}

void music_player(void) {
	if (argc != 1) {
		printf(ARG_COUNT_ERROR);
		return;
	}
	int note_input;
	int mult = 1;
	change_colour_current(7,1);
	terminal_initialize();
	printf("PC Speaker Music Player\r\n");
	printf("For list of notes, type ctrl+H\r\n");
	while (1) {
		note_input = get_key();
		switch (note_input) {
			case 0x11:
				nosound();
				return;
			case '1':
				mult = 1;
				break;
			case '2':
				mult = 2;
				break;
			case '3':
				mult = 3;
				break;
			case 'q':
				play_sound(130*mult);
				break;
			case 'w':
				play_sound(139*mult);
				break;
			case 'e':
				play_sound(147*mult);
				break;
			case 'r':
				play_sound(156*mult);
				break;
			case 't':
				play_sound(165*mult);
				break;
			case 'y':
				play_sound(175*mult);
				break;
			case 'u':
				play_sound(185*mult);
				break;
			case 'i':
				play_sound(196*mult);
				break;
			case 'o':
				play_sound(208*mult);
				break;
			case 'p':
				play_sound(220*mult);
				break;
			case '[':
				play_sound(233*mult);
				break;
			case ']':
				play_sound(247*mult);
				break;
		}
	}
}
