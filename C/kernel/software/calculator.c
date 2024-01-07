#include<tty.h>
#include<stdio.h>
#include<stdint.h>

void calculator(void) {
	char operand1[10];
	char operator[1];
	char operand2[10];
	uint8_t input_char;
	terminal_initialize();
	printf("MOSS calculator\r\n");
	while(1) {
		memset(operand1, 0, sizeof(operand1));
		printf("Type operand:");
		while(1) {
			input_char = get_key();
			if (input_char == 0x0D) {
				printf("\r\n");
				break;
			}
			if (input_char == 0x08) {
				if (input_length > 0) {
					cmd_str[--input_length] = '\0';
				}
			}
		}
	}

}
