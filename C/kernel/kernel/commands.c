// MOSS KERNEL - Shell Commands
// (C) Miro Haapalainen, 2024

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <kernel/kernel.h>
#include <kernel/sched.h>

extern Inode inodeList[1024];
extern char fileNames[1024][32];
extern mfs_file files[1024];
extern mfs_dir dirs[1024];
extern size_t inodeCount;
extern uint32_t currentInode;
extern int colour_scheme[];
extern int custom_colour_scheme;

/* Defined in fs.c */
extern int isFile(char* name);
extern void touch_cmd(void);

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
		printf("ps               View currently running processes\r\n"); 
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

void ps_cmd(void) {
	int count = get_task_count();
	printf("PID   TIME       CMD\r\n");
	printf("0     00:00:00   kernelsys\r\n");
	printf("1     00:00:00   shell\r\n");
	printf("Total tasks in scheduler: %d\r\n", count);
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
	printf("Type 'CTRL+s' to write to file\r\n");
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
	while (1) {
		tex_input = get_key();
		if (tex_input == 0x11) {
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
		}
		putchar(tex_input);
		tex_buffer[tex_input_length++] = tex_input;
	}
}

void tex_print(char* text) {
	printf(text);
}

void run_file(void) {
	if (argc != 2) {
		printf(ARG_COUNT_ERROR);
		return;
	}
	int id = isFile(argv[1]);
	if (id == -1) {
		printf(FILE_NOT_FOUND_ERROR);
		return;
	}
	char* token = strtok((char *)files[id].data, " ");
	while (token != NULL) {
		if (memcmp(token, "goto", strlen("goto")) == 0) {
			
		}
		if (memcmp(token, "print", strlen("print")) == 0) {
			tex_print(strtok(NULL, " "));
		}
		token = strtok(NULL, " ");
	}
}

void snake_game(void) {
	terminal_initialize();
	update_cursor(3, 2);
	const char snake_cursor[] = {178, '\0'};
	printf(snake_cursor);
}
