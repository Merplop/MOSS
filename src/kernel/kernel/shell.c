// MOSS KERNEL - Shell & Language Prompt
// (C) Miro Haapalainen, 2024

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <kernel/kernel.h>
#include <kernel/ext2.h>
#include <kernel/elf.h>
#include <kernel/users.h>
#include <moss/commands.h>

extern uint32_t cwd_ino;
extern char cwd_path[256];
extern int colour_scheme[];
extern int custom_colour_scheme;

/* Defined in fs.c */
extern void ls_cmd(void);
extern void mkdir_cmd(void);
extern void touch_cmd(void);
extern void cd_cmd(void);
extern void mv_cmd(void);
extern void rm_cmd(void);
extern void cat_cmd(void);
extern void rmdir_cmd(void);
extern void disks_cmd(void);
extern void mkfs_cmd(void);
extern void mount_cmd(void);
extern uint32_t fs_find_file(const char *name);

/* Defined in commands.c */
extern void shutdown_cmd(void);
extern void colour_cmd(void);
extern void sleep_cmd(void);
extern void help_cmd(void);
extern void clear_cmd(void);
extern void fetch_cmd(void);
extern void ps_cmd(void);
extern void text_editor(void);
extern void run_file(void);
extern void snake_game(void);
extern void mmap_cmd(void);
extern void priv_cmd(void);
extern void exec_cmd(void);

/* Defined in sound.c */
extern void music_player(void);
extern void nosound(void);

/* Defined in net.c */
extern void ping_cmd(void);
extern void resolve_cmd(void);

/* Defined in tcp.c */
extern void wget_cmd(void);

/* Defined in browse.c */
extern void browse_cmd(void);

/* Defined in commands.c (user management) */
extern void whoami_cmd(void);
extern void useradd_cmd(void);
extern void userdel_cmd(void);
extern void passwd_cmd(void);
extern void users_cmd(void);
extern void id_cmd(void);
extern void su_cmd(void);

char* commands[NUM_COMMANDS] = {"sound", "stopsound", "tex", "cat", "reb", "hlt", "shutdown",
"colour", "sleep", "help", "clear", "fetch", "ps", "touch", "mkdir", "cd", "mv", "rm", "cmp", "priv", "mmap", "exec",
"disks", "mkfs", "mount", "ping", "resolve", "wget", "browse", "whoami", "useradd", "userdel", "passwd", "users", "id", "su"};
void (*command_ptrs[NUM_COMMANDS])() = {music_player, nosound, text_editor, cat_cmd, reb_cmd, hlt_cmd, shutdown_cmd,
colour_cmd, sleep_cmd, help_cmd, clear_cmd, fetch_cmd, ps_cmd, touch_cmd, mkdir_cmd, cd_cmd, mv_cmd, rm_cmd, run_file, priv_cmd, mmap_cmd, exec_cmd,
disks_cmd, mkfs_cmd, mount_cmd, ping_cmd, resolve_cmd, wget_cmd, browse_cmd, whoami_cmd, useradd_cmd, userdel_cmd, passwd_cmd, users_cmd, id_cmd, su_cmd};

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

void start_shell(void) {
	enable_cursor(0, 15);
	login_prompt();
	while(1) {
		memset(cmd_str, 0, sizeof(cmd_str));
		input_length = 0;
		memset(argv, 0, sizeof(argv));
		argc = 0;
		if (custom_colour_scheme == 0) {
			change_colour_current(3, 0);
			printf("%s@%s", user_current_name(), cwd_path);
			change_colour_current(15, 0);
			printf(prompt);
			change_colour_current(7, 0); 
		} else {
			printf("%s@%s", user_current_name(), cwd_path);
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
			if (memcmp(arg_token, commands[i], strlen(arg_token)) == 0 && 
			memcmp(arg_token, commands[i], strlen(commands[i])) == 0) {
				while (arg_token != NULL) {
					if (argc == MAX_ARGS) {
						printf(ARG_COUNT_ERROR);
						arg_error_caught = 1;
						break;
					}
					argv[argc++] = arg_token;
					arg_token = strtok(NULL, " ");
				}
				if (arg_error_caught) {break;}
				(*command_ptrs[i])();
				command_found = 1;
				break;
			}
		}
		if (command_found == 0 && arg_token != NULL) {
			/* Try to find and execute an ELF binary in /bin/ */
			char bin_path[270];
			memset(bin_path, 0, sizeof(bin_path));
			strcpy(bin_path, "/bin/");
			strncat(bin_path, arg_token, sizeof(bin_path) - 6);

			/* Parse remaining arguments */
			while (arg_token != NULL) {
				if (argc == MAX_ARGS) {
					printf(ARG_COUNT_ERROR);
					arg_error_caught = 1;
					break;
				}
				argv[argc++] = arg_token;
				arg_token = strtok(NULL, " ");
			}

			if (!arg_error_caught) {
				uint32_t file_ino = fs_find_file(bin_path);
				if (file_ino != 0) {
					elf_load_and_exec(file_ino, argc, argv);
				} else {
					printf(CMD_ERROR);
				}
			}
		}
	}
}
