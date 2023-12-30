#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <sys/io.h>

#define NUM_COMMANDS 9

const char version[] = "Beta 1.0";
const char welcome1[] = "------------------------------\r\nMOSS Kernel - Version ";
const char welcome2[] = "\r\n------------------------------\r\n";
const char prompt[] = ">";
char cmd_str[256];
char *cmd_str_ptr = cmd_str;
uint8_t input_char = 0;
uint8_t input_length;
int argc = 0;
char *argv[10] = {0};

void dir_cmd(void) {
	printf("TODO: Implement file table so this command can work!\r\n");
}

void reb_cmd(void) {
	uint8_t g = 0x02;
	while (g & 0x02) {
		g = inb(0x64);
	}
	outb(0x64, 0xFE);
	__asm__ __volatile__("hlt");
}

void hlt_cmd(void) {
	printf("Halting the CPU - you will have to hard reboot your PC");
	__asm__ __volatile__("hlt");
}

void shutdown_cmd(void) {
	printf("TODO: Implement outw syscall\r\n");
}

void colour_cmd(void) {
	printf("TODO: Implement helper method in tty.c\r\n");
}

void sleep_cmd(void) {
	printf("TODO: Write this thang\r\n");
}

void help_cmd(void) {
	printf("List of internal kernel commands\r\n");
	printf("dir              List contents of current directory\r\n");
	printf("reb              Reboot computer\r\n");
	printf("hlt              Halt CPU\r\n");
	printf("shutdown         Shutdown computer\r\n");
	printf("colour <fg> <bg> Change terminal colour\r\n");
	printf("sleep <s>        Sleep for s seconds\r\n");
	printf("help             Display this\r\n");
	printf("clear            Clear screen\r\n");
}

void clear_cmd(void) {
	terminal_initialize();
}

void ver_cmd(void) {
	printf("Kernel version %s\r\n", version);
	printf("GNU General Public License v3.0\r\nC. Miro Haapalainen, 2024\r\n");
}

char* commands[NUM_COMMANDS] = {"dir", "reb", "hlt", "shutdown", "colour", "sleep", "help", "clear", "ver"};
void (*command_ptrs[NUM_COMMANDS])(void) = {dir_cmd, reb_cmd, hlt_cmd, shutdown_cmd, colour_cmd, sleep_cmd, help_cmd, clear_cmd, ver_cmd};


void kernel_main(void) {
	terminal_initialize();
	printf(welcome1);
	printf(version);
	printf(welcome2);
	
	while(1) {
		memset(cmd_str, 0, sizeof(cmd_str));
		input_length = 0;
		printf(prompt);
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
			// TODO - handle ASCII control characters,
			// however the fuck you do that
			if (input_char == 0x0C) {
				clear_cmd();
				break;
			}
			cmd_str[input_length++] = input_char;
			putchar(input_char);
			
		}
		
		int command_found = 0;
	        for (int i = 0; i < NUM_COMMANDS; i++) {
			// TODO - the memcmp call below only goes until the length of
			// commands[i], so if you inputted helpgjdjkfgjldkfg, it would
			// run the 'help' command.
			if (memcmp(cmd_str, commands[i], sizeof(commands[i])) == 0) {
				(*command_ptrs[i])();
				command_found = 1;
				break;
			}
		}
		if (command_found == 0) {
			printf("ERROR: Command not found\r\n");
		}
	}
}

