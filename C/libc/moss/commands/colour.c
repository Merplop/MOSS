/*#define MAX_ARGS 10
#include <stdlib.h>
#include<stdint.h>
#include<stdio.h>
#include <kernel/kernel.h>

void colour_cmd(char *argv[MAX_ARGS], int argc) {
	if (argc == 2) {
		if (memcmp(argv[1], "-h", sizeof("-h")) == 0) {
			printf("USAGE: colour [FOREGROUND] [BACKGROUND]\r\n");
			printf("Changes terminal colour scheme\r\n");
			printf("For list of available colours, type 'colour -l'\r\n");
		} else if (memcmp(argv[1], "-l", sizeof("-l")) == 0) {
			terminal_initialize();
			printf("List of available colours:\r\n");
			printf("0 - Black\r\n");
			printf("1 - Blue\r\n");
			printf("2 - Green\r\n");
			printf("3 - Cyan\r\n");void
			printf("4 - Red\r\n");
			printf("5 - Magenta\r\n");
			printf("6 - Brown\r\n");
			printf("7 - Light Grey\r\n");
			printf("8 - Dark Grey\r\n");
			printf("9 - Light Blue\r\n");
			printf("10 - Light Green\r\n");
			printf("11 - Light Cyan\r\n");
			printf("12 - Light Red\r\n");
			printf("13 - Light Magenta\r\n");
			printf("14 - Light Brown\r\n");
			printf("15 - White\r\n");
		} else {
			printf(ARG_ERROR);
		}
	} else if (argc == 3) {
		uint8_t fg = atoi(argv[1]);
		uint8_t bg = atoi(argv[2]);
		if ((fg > 15) || (bg > 15)) {
			printf(ARG_ERROR);
			return;
		}
		colour_scheme[0] = fg;
		colour_scheme[1] = bg;
		change_colour(fg, bg);
		custom_colour_scheme = 1;
	} else {
		printf(ARG_COUNT_ERROR);
		return;
	}
}*/