#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <kernel/kernel.h>
#include <sys/io.h>
#include <sys/sleep.h>
//#define NUM_COMMANDS 17
//#define MAX_ARGS 10

Inode inodeList[1024];
char fileNames[1024][32];
mfs_file files[1024];
mfs_dir dirs[1024];
process processes[1024];
size_t inodeCount = 1;
uint32_t currentInode = 0;

int colour_scheme[] = {7, 0};
int custom_colour_scheme = 0;



int isdigit(char c) { return c >= '0' && c <= '9'; }

int atoi(const char *str) {
  uint32_t value = 0;
  while (isdigit(*str)) {
    value *= 10;
    value += (*str) - '0';
    str++;
  }

  return value;
}


static void play_sound(uint32_t nFrequence) {
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

void enable_speaker() {
	uint8_t temp = inb(0x61);
	outb(0x61, temp | 3);
}

void disable_speaker() {
	uint8_t temp = inb(0x61);
	outb(0x61, temp & 0xFC);
}

static void nosound() {
	uint8_t tmp = inb(0x61) & 0xFC;
	outb(0x61, tmp);
}

void beep_cmd() {
	if (argc != 2) {
		printf(ARG_COUNT_ERROR);
		return;
	}
	int freq = atoi(argv[1]);
	play_sound(freq);
}


int isFile(char* name) {
	for (int i=0;i<1024;i++) {
		if (inodeList[i].parent == currentInode && memcmp(fileNames[i], name, strlen(name)) == 0) {
			return i;
		}
	}
	return -1;
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
			printf("3 - Cyan\r\n");
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
}

void sleep_cmd(void) {
	printf("TODO: Write this thang\r\n");
}

void help_cmd(void) {
	if (language == 0) {
		printf("List of internal kernel commands\r\n");
		printf("fetch            Display kernel information\r\n");
		printf("ls               List contents of current directory\r\n");
		printf("reb              Reboot computer\r\n");
		printf("hlt              Halt CPU\r\n");
		printf("shutdown         Shutdown computer\r\n");
		printf("colour <fg> <bg> Change terminal colour\r\n");
		printf("sleep <s>        Sleep for s seconds\r\n");
		printf("help             Display this\r\n");
		printf("clear            Clear screen\r\n");
		printf("ps				 View currently running processes\r\n"); 
		printf("touch <name>     Create a file in working directory\r\n");
		printf("mkdir <name>     Create a directory in working directory\r\n");
		printf("cd <directory>   Change working directory\r\n");
		printf("mv <file> <dir>  Move a file/directory into another directory\r\n");
		printf("tex <filename>   Edit a MOSS-formatted data file\r\n");
		printf("rm <filename>    Remove a file or directory\r\n");
		printf("mv <filename>    Move a file or directory\r\n");
	} else if (language == 1) {
		printf("Sis"); 
		putchar(132);
		printf("isesti m"); putchar(132); putchar(132);
		printf("ritelty komennot\r\n");
                printf("ls               N"); putchar(132);
		printf("yt");
		putchar(132); 
		printf(" nykyisen hakemiston sis");
		putchar(132);
		printf("ll");
		putchar(148);
		printf("n\r\n");
                printf("reb              K");
		putchar(132);
		printf("ynnist");
		putchar(132);
		printf("tietokone uudelleen\r\n");
                printf("hlt              Pys");
		putchar(132);
		printf("yt");
	        putchar(132);
	 	printf(" suoritin\r\n");
                printf("shutdown         Sammuta tietokone\r\n");
		printf("colour <fg> <bg> Vaihda p");
		putchar(132);
		putchar(132);
		printf("tteen v");
		putchar(132);
		printf("ri");
		putchar(132);
		printf("\r\n");
                printf("sleep <s>        Lep");
		putchar(132);
	        putchar(132);	
		printf(" s sekunttia\r\n");
                printf("help             N");
		putchar(132);
		printf("yt");
		putchar(132);
	        printf(" t");
		putchar(132);
		putchar('m');
		putchar(132);
		printf("\r\n");
                printf("clear            Tyhjenn");
		putchar(132);
		printf(" p");
		putchar(132);
		putchar(132);
		printf("te\r\n");
		printf("exec <ohjelma>   Suorita ohjelma\r\n");
		printf("touch <nimi>     Luo uusi tiedosto\r\n");
		printf("mkdir <nimi>     Luo uusi hakemisto\r\n");
		printf("cd <directory>   Vaihda hakemistoa\r\n");
		printf("mv <tied> <hak>  Siirr");
		putchar(132);
		printf(" tiedosto/hakemisto toiseen hakemistoon\r\n");
		printf("tex <nimi>       Muokkaa MOSSissa alustettua tiedostoa\r\n");
		printf("rm <nimi>        Poista tiedosto/hakemisto\r\n");
		printf("mv <nimi>        Siirr");
		putchar(132);
		printf(" tiedosto/hakemisto\r\n");
	}
}

void clear_cmd(void) {
	if (argc > 1) {
		printf(ARG_COUNT_ERROR);
		return;
	}
	terminal_initialize();
}

void fetch_cmd(void) {
	if (argc > 1) {
		printf(ARG_COUNT_ERROR);
		return;
	}
	if (language == 0) {
		printf("Kernel version %s\r\n", version);
		printf("GNU General Public License v3.0\r\nC. Miro Haapalainen, 2024\r\n");
		printf("MFS (MOSS Filesystem), working directory '%s'\r\n", fileNames[currentInode]);
	} else if (language == 1) {
		printf("Kernelin versio %s\r\n", version);
		printf("GNU GPL-julkinen lisenssi v3.0\r\nC. Miro Haapalainen, 2024\r\n");
		printf("MFS (MOSS-tiedostojärjestelmä), nykyinen hakemisto '%s'\r\n", fileNames[currentInode]);
	}
}

void ls_cmd(void) {
	printf(".\r\n..\r\n");
	for (int i=0;i<1024;i++) {
		if (inodeList[i].parent == currentInode && (inodeList[i].type == 'd' || inodeList[i].type == 'f') && inodeList[i].number != currentInode) {
			if (inodeList[i].type == 'd') {
				printf("[%s]   ", fileNames[inodeList[i].number]);
				printf("%d\0", dirs[i].size);
				printf(" bytes\r\n");
			} else {
				printf("%s ", fileNames[inodeList[i].number]);
				printf("%s   ", files[i].type);
				printf("%d\0", files[i].size);
				printf(" bytes\r\n");
			}
		}
	}
}

void mkdir_cmd() {
	if (argc != 2) {
		printf(ARG_COUNT_ERROR);
		return; 
	}
        if (inodeCount == 1024) {
                printf(INODE_FULL_ERROR);
                return;
        }
        for (int i=0;i<1024;i++) {
                if (memcmp(&fileNames[inodeList[i].number], argv[1], strlen(argv[1])) == 0 && inodeList[i].parent == currentInode) {
                        printf(DIR_EXISTS_ERROR);
                        return;
                }
        }
        Inode i1;
	i1.type = 'd';
        i1.number = inodeCount;
	memcpy(&fileNames[inodeCount], argv[1], strlen(argv[1]));
        i1.parent = currentInode;
	mfs_dir d1;
	d1.size = 0;
	dirs[inodeCount] = d1;
        inodeList[inodeCount++] = i1;
}

void touch_cmd(void) {
	if (argc != 2) {
		printf(ARG_COUNT_ERROR);
		return;
	}
	if (inodeCount == 1024) {
		printf(INODE_FULL_ERROR);
	}
	for (int i=0;i<1024;i++) {
		if (inodeList[i].parent == currentInode && memcmp(fileNames[inodeList[i].number], argv[1], strlen(argv[1])) == 0) {
			printf(FILE_EXISTS_ERROR);
			return;
		}
	}
	Inode i1;
	i1.number = inodeCount;
	i1.type = 'f';
	memcpy(&fileNames[inodeCount], argv[1], strlen(argv[1]));
	i1.parent = currentInode;
	mfs_file f1;
	files[inodeCount] = f1;
/*	if (argc > 2) {
		int k=0;
		for (int i=3;i<argc;i++) {
			for (int j=0;j<strlen(argv[i]);j++) {
				files[inodeCount].data[k] = argv[i][j];
				k++;
				f1.size++;
			}
			f1.data[k++] = ' ';
		}
		f1.data[k] = '\0';
	} */
	files[inodeCount].size = 0;
	memcpy(&files[inodeCount].type, "MDF", strlen("MDF"));
	inodeList[inodeCount++] = i1;
}

void cd_cmd(void) {
	if (argc == 1) {
		currentInode = 0;
	} else if (argc == 2) {
		if (memcmp(argv[1], "..", strlen("..")) == 0) {
			currentInode = inodeList[currentInode].parent;
		} else {
			for (int i = 0; i < 1024; i++) {
				if (inodeList[i].parent == currentInode && memcmp(fileNames[inodeList[i].number], argv[1], strlen(argv[1]))== 0) {
					if (inodeList[i].type != 'd') {
						printf(ARG_ERROR);
						return;
					}
					currentInode = i;
				}
			}	
		} 
	} else {
		printf(ARG_COUNT_ERROR);
	}
}

void mv_cmd(void) {

}

void rm_cmd(void) {
	if (argc != 2) {
		printf(ARG_COUNT_ERROR);
		return;
	}
	int id = isFile(argv[1]);
	if (id == -1) {
		printf(FILE_NOT_FOUND_ERROR);
		return;
	}
	
}

void cat_cmd(void) {
	if (argc == 1) {
		// TODO	
	} else if (argc == 2) {
		int id = isFile(argv[1]);
		if (id != -1) {
			int len = strlen(files[id].data);
			if (len == 0) {
				printf(FILE_EMPTY_ERROR);
				return;
			}
			for (int i=0;i<len;i++) {
				putchar(files[id].data[i]);
			}
			printf("\r\n");
		} else {
			printf(FILE_NOT_FOUND_ERROR);
			return;
		}
	} else {
		printf(ARG_COUNT_ERROR);
		return;
	}
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
			case 'q':
				play_sound(41*mult);
				break;
			case 'w':
				play_sound(43*mult);
				break;
			case 'e':
				play_sound(46*mult);
				break;
			case 'r':
				play_sound(49*mult);
				break;
			case 't':
				play_sound(52*mult);
				break;
			case 'y':
				play_sound(55*mult);
				break;
			case 'u':
				play_sound(58*mult);
				break;
			case 'i':
				play_sound(61*mult);
				break;
			case 'o':
				play_sound(65*mult);
				break;
			case 'p':
				play_sound(69*mult);
				break;
		}
		if (note_input == 'q') {
			play_sound(82);
		} else if (note_input == 'w') {
			play_sound(87);
		} else if (note_input == 'e') {
			play_sound(92);
		} else if (note_input == 'r') {
			play_sound(98);
		}
	}
}

void ps_cmd() {
	for (int i=0; i<process_count; i++) {
		printf("%d   ", processes[i].pid);
		printf("00:00:00   %s\r\n", processes[i].cmd);
	}
}


void text_editor(void) {
	if (argc != 2) {
		printf(ARG_COUNT_ERROR);
		return;
	}
	change_colour_current(7, 1);
	terminal_initialize();
	printf("=======================================================\r\n");
	printf("MOSS DATA FILE EDITOR - %s.MDF\r\n", argv[1]);
	printf("=======================================================\r\n");
	printf("Type ';w' to write to file\r\n");
	int id = isFile(argv[1]);
	char tex_buffer[8192];
	int tex_input_length = 0;
	if (id != -1) {
		for (int i=0;files[id].data[i] != '\0';i++) {
			tex_buffer[i] = files[id].data[i];
			putchar(files[id].data[i]);
			tex_input_length++;
		}
	}
	char tex_input;
	int option_mode = 0;
	while (1) {
		tex_input = get_key();
		if (option_mode == 1) {
			if (tex_input == 'w') {
				if (id == -1) {
					argv[0] = "touch";
					touch_cmd();
					id = isFile(argv[1]);
				}
				for (int i=0;i<tex_input_length;i++) {
					files[id].data[i] = tex_buffer[i];
				}
				files[id].data[tex_input_length] = '\0';
				files[id].size = tex_input_length;
				change_colour(colour_scheme[0], colour_scheme[1]);
				terminal_initialize();
				return;
			}
		}
		if (tex_input == 0x0D) {
			putchar('\r');
			putchar('\n');
			tex_buffer[tex_input_length++] = '\r';
			tex_buffer[tex_input_length++] = '\n';
			continue;
		} else if (tex_input == 0x08) {
			if (tex_input_length > 0) {
				tex_buffer[--tex_input_length] = '\0';
				putchar(tex_input);
			}
			continue;
		} else if (tex_input == ';') {
			option_mode = 1;
			continue;
		}
		putchar(tex_input);
		tex_buffer[tex_input_length++] = tex_input;
	}

}

char* commands[NUM_COMMANDS] = {"sound", "stopsound", "tex", "cat", "ls", "reb", "hlt", "shutdown", 
"colour", "sleep", "help", "clear", "fetch", "ps", "touch", "mkdir", "cd"};
void (*command_ptrs[NUM_COMMANDS])(void) = {beep_cmd, nosound, text_editor, cat_cmd, ls_cmd, reb_cmd, hlt_cmd, shutdown_cmd, 
colour_cmd, sleep_cmd, help_cmd, clear_cmd, fetch_cmd, ps_cmd, touch_cmd, mkdir_cmd, cd_cmd};


void language_prompt(void) {
	printf(language_spacer_top);
        printf(language_spacer_side);
        printf(language_box_top);
        printf(language_spacer_side);
        printf(box_side);
        printf(language_box_title);
        printf(box_side);
        printf(newline);
        printf(language_spacer_side);
        printf(language_box_middle);
	printf(language_spacer_side);
	printf(box_side);
	printf(language_option1);
	printf(box_side);
	printf(newline);
	printf(language_spacer_side);
	printf(box_side);
	printf(language_option2);
	printf(box_side);
	printf(newline);
	printf(language_spacer_side);
	printf(language_box_bottom);
	memset(cmd_str, 0, sizeof(cmd_str));
	while (1) {
		input_char = get_key();
		if (input_char == 0x31) {
			language = 0;
			break;
		} else if (input_char == 0x32) {
			language = 1;
			break;
		}
	}
}

void start_shell() {
	process shell;
	shell.pid = 1;
	shell.parent = &processes[0];
	memcpy(&shell.cmd, "shell\0", strlen("shell\0"));
	processes[process_count++] = shell;
	enable_cursor();
	while(1) {
		memset(cmd_str, 0, sizeof(cmd_str));
		input_length = 0;
		memset(argv, 0, sizeof(argv));
		argc = 0;
		if (custom_colour_scheme == 0) {
			change_colour_current(3, 0);
			printf(fileNames[currentInode]);
			printf(":/");
			change_colour_current(15, 0);
			printf(prompt);
			change_colour_current(7, 0); 
		} else {
			printf(fileNames[currentInode]);
			printf(":/");
			printf(prompt);
		}
		while(1) {
			input_char = get_key();
			if (input_char == 0x0D) {
				printf("\r\n");
				break;
			}
			if (input_char == 0x08) {
				if (input_length > 0) {
					cmd_str[--input_length] = '\0';
					putchar(input_char);
				}
				continue;
			}
			if (input_char == 0x0C) {
				clear_cmd();
				break;
			}
			cmd_str[input_length++] = input_char;
			putchar(input_char);
		}
		char clear_ctrl_cmd[] = {0x0C};
		if (memcmp(clear_ctrl_cmd, cmd_str, strlen(cmd_str)) == 0) {
			continue;
		}
		arg_token = strtok(cmd_str, " ");
		int command_found = 0;
		int arg_error_caught = 0;
	        for (int i = 0; i < NUM_COMMANDS; i++) {
			if (memcmp(arg_token, commands[i], strlen(arg_token)) == 0 && memcmp(arg_token, commands[i], strlen(commands[i])) == 0) {
				while (arg_token != NULL) {
					if (argc == MAX_ARGS) {
						printf(ARG_COUNT_ERROR);
						arg_error_caught = 1;
						break;
					}
					argv[argc++] = arg_token;
					arg_token = strtok(NULL, " ");
			//		argv[argc++] = arg_token;
				}
				if (arg_error_caught) {break;}
				(*command_ptrs[i])();
				command_found = 1;
				break;
			}
		}
		if (command_found == 0) {
			printf(CMD_ERROR);
		}

	}
}

void kernel_main(void) {
	change_colour(7, 1);
	memcpy(&fileNames[0], "root", strlen("root"));
	dirs[0].size = 0;
	process system;
	system.pid = 0;
	system.parent = &system;
	memcpy(&system.cmd, "kernelsys\0", strlen("kernelsys\0"));
	processes[process_count++] = system;
	terminal_initialize();
	disable_cursor();
	language_prompt();
	change_colour(7, 0);
	terminal_initialize();
	if (language == 0) {
		printf(welcome1_eng);
		printf(version);
		printf(welcome2); 
	} else if (language == 1) {
		printf(welcome1_fin);
		printf(version);
		printf(welcome2);
	}
	start_shell();
}

