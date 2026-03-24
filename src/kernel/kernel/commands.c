// MOSS KERNEL - Shell Commands
// (C) Miro Haapalainen, 2024

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <kernel/kernel.h>
#include <kernel/sched.h>
#include <kernel/ext2.h>
#include <sys/multiboot.h>

extern uint32_t cwd_ino;
extern int colour_scheme[];
extern int custom_colour_scheme;

/* Defined in fs.c */
extern uint32_t fs_find_file(const char *name);
extern void touch_cmd(void);

void mmap_cmd(void) {
	printf("MEMORY MAP:\r\n");
	for (uint32_t i = 0; i < g_mmap_len; i += sizeof(multiboot_memory_map_t)) {
		multiboot_memory_map_t* mmmt = (multiboot_memory_map_t*) (g_saved_mmap + i);

		printf("Start addr: ");
		print_hex(mmmt->addr);
		printf(" | Length: ");
		print_hex(mmmt->len);
		printf(" | Size: ");
		print_hex(mmmt->size);
		printf(" | Type : ");
		if (mmmt->type == MULTIBOOT_MEMORY_AVAILABLE) {
			printf("Available");
		} else if (mmmt->type == MULTIBOOT_MEMORY_RESERVED) {
			printf("Reserved");
		} else if (mmmt->type == MULTIBOOT_MEMORY_ACPI_RECLAIMABLE) {
			printf("ACPI-Reclaimable");
		} else if (mmmt->type == MULTIBOOT_MEMORY_NVS) {
			printf("Non-volatile storage");
		} else if (mmmt->type == MULTIBOOT_MEMORY_BADRAM) {
			printf("Faulty RAM");
		}
		printf("\r\n");
	}
}

void priv_cmd(void) {
	extern int kernel_privilege;
	if (argc < 2) {
		printf(ARG_COUNT_ERROR);
		return;
	}
	if (argc == 3) {
		if (memcmp(argv[1], "get", sizeof("get")) == 0) {
			printf("Current privilege level: %d\r\n", kernel_privilege);
			return;
		} else if (memcmp(argv[1], "set", sizeof("set")) == 0) {
			if (memcmp(argv[2], "test", sizeof("test")) == 0) { 
				kernel_privilege = 1;
			}
		} else {
			printf(ARG_ERROR);
			return;
		}
		int level = atoi(argv[2]);
		if (level < 0 || level > 3) {
			printf(ARG_ERROR);
			return;
		}
		kernel_privilege = level;
	} 
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
	printf("List of internal kernel commands\r\n");
	printf("fetch              Display kernel information\r\n");
	printf("ls                 List contents of current directory\r\n");
	printf("reb                Reboot computer\r\n");
	printf("hlt                Halt CPU\r\n");
	printf("shutdown           Shutdown computer\r\n");
	printf("colour <fg> <bg>   Change terminal colour\r\n");
	printf("sleep <s>          Sleep for s seconds\r\n");
	printf("help               Display this\r\n");
	printf("clear              Clear screen\r\n");
	printf("ps                 View currently running processes\r\n"); 
	printf("touch <name>       Create a file in working directory\r\n");
	printf("mkdir <name>       Create a directory in working directory\r\n");
	printf("cd <directory>     Change working directory\r\n");
	printf("mv <file> <dir>    Move a file/directory into another directory\r\n");
	printf("tex <filename>     Edit a MOSS-formatted data file\r\n");
	printf("rm <filename>      Remove a file or directory\r\n");
	printf("mv <filename>      Move a file or directory\r\n");
	printf("priv <opt> [priv]  Set kernel privilege level\r\n");
	printf("mmap               Display memory map\r\n");
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
	printf("Kernel version %s\r\n", version);
	printf("GNU General Public License v3.0\r\nC. Miro Haapalainen, 2024\r\n");
	printf("ext2 filesystem, CWD inode %d\r\n", cwd_ino);
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
	printf("MOSS DATA FILE EDITOR - %s\r\n", argv[1]);
	printf("=======================================================\r\n");
	printf("Type 'CTRL+s' to write to file\r\n");

	uint32_t file_ino = fs_find_file(argv[1]);
	char tex_buffer[8192];
	int tex_input_length = 0;
	memset(tex_buffer, 0, sizeof(tex_buffer));

	if (file_ino != 0) {
		/* Load existing file contents */
		ext2_inode_t ino;
		if (ext2_read_inode(file_ino, &ino) == 0 && ino.i_size > 0) {
			uint32_t to_read = ino.i_size;
			if (to_read > sizeof(tex_buffer) - 1)
				to_read = sizeof(tex_buffer) - 1;
			int rd = ext2_read_file(file_ino, tex_buffer, 0, to_read);
			if (rd > 0) {
				tex_input_length = rd;
				for (int i = 0; i < rd; i++)
					putchar(tex_buffer[i]);
			}
		}
	}

	char tex_input;
	while (1) {
		tex_input = get_key();
		if (tex_input == 0x11) {
			/* CTRL+S: save */
			if (file_ino == 0) {
				/* Create the file first */
				argv[0] = "touch";
				touch_cmd();
				file_ino = fs_find_file(argv[1]);
			}
			if (file_ino != 0) {
				ext2_truncate(file_ino);
				if (tex_input_length > 0) {
					ext2_write_file(file_ino, tex_buffer, 0, tex_input_length);
				}
			}
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
	uint32_t file_ino = fs_find_file(argv[1]);
	if (file_ino == 0) {
		printf(FILE_NOT_FOUND_ERROR);
		return;
	}
	ext2_inode_t ino;
	if (ext2_read_inode(file_ino, &ino) != 0 || ino.i_size == 0)
		return;
	uint32_t sz = ino.i_size;
	if (sz > 8191) sz = 8191;
	char file_data[8192];
	memset(file_data, 0, sizeof(file_data));
	ext2_read_file(file_ino, file_data, 0, sz);

	char* token = strtok(file_data, " ");
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
