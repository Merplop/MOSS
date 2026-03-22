// MOSS KERNEL
// (C) Miro Haapalainen, 2024
//

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <kernel/kernel.h>
#include <kernel/memory_manager.h>
#include <kernel/sched.h>
#include <sys/io.h>
#include <sys/sleep.h>
#include <moss/commands.h>
#include <sys/multiboot.h>

/* Arch-specific interrupt and timer support */
void idt_init(void);
void timer_init(uint32_t frequency);

/* --- Global variable definitions (declared extern in kernel.h) --- */

const char KERNEL_PANIC_ERROR[] = "ERROR 00 - Kernel panic error\r\n";
const char CMD_ERROR[] = "ERROR 01 - Command not found\r\n";
const char ARG_COUNT_ERROR[] = "ERROR 02 - Invalid number of command-line arguments\r\n";
const char ARG_ERROR[] = "ERROR 03 - Invalid command-line arguments\r\n";
const char INODE_FULL_ERROR[] = "ERROR 04 - Inode list full\r\n";
const char DIR_EXISTS_ERROR[] = "ERROR 05 - Directory already exists\r\n";
const char FILE_EXISTS_ERROR[] = "ERROR 06 - File already exists\r\n";
const char FILE_NOT_FOUND_ERROR[] = "ERROR 07 - File not found\r\n";
const char FILE_EMPTY_ERROR[] = "ERROR 08 - File is empty\r\n";
const char DIR_NOT_FOUND_ERROR[] = "ERROR 09 - Directory not found\r\n";
const char KERNEL_ERROR_FIN[] = "VIRHE 00 - Kerneli-paniikki\r\n";
const char CMD_ERROR_FIN[] = {'V','I','R','H','E',' ','0','1',' ','-',' ','k','o','m','e','n','t','o','a',' ','e','i',' ','l',148,'y','t','y','n','y','t','\r','\n'};
const char ARG_COUNT_ERROR_FIN[] = {'V','I','R','H','E',' ','0','2',' ','-',' ','V',132,132,'r',132,' ','m',132,132,' ','r',132,' ','a','r','g','u','m','e','n','t','t','e','j','a','\r','\n'};

const char version[] = "Beta 1.1";
const char welcome1_eng[] = "------------------------------\r\nMOSS Kernel - Version ";
const char welcome1_fin[] = "------------------------------\r\nMOSS-kerneli - Versio ";
const char welcome2[] = "\r\n------------------------------\r\n";
const char prompt[] = ">";
char cmd_str[256];
char *cmd_str_ptr = cmd_str;
uint8_t input_char = 0;
uint8_t input_length;
int argc = 0;
char *argv[MAX_ARGS] = {0};
int language = 0;
const char newline[] = "\r\n";
const char language_spacer_side[18] = {32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, 32, '\0'};
const char language_spacer_top[] = "\r\n\r\n\r\n\r\n";
const char language_box_top[46] = {201,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,187,'\r', '\n', '\0'};
const char language_box_title[] = "      MOSS Kernel - Choose Language      ";
const char language_box_middle[46] = {204,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,185,'\r', '\n', '\0'}; 
const char box_side[2] = {186, '\0'};
const char language_box_bottom[46] = {200,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,205,188,'\r', '\n', '\0'};
const char language_option1[] = "  1. English                             ";
const char language_option2[] = "  2. suomi                               ";
char* arg_token;
int process_count = 0;
int current_process;

/* --- Filesystem globals --- */

Inode inodeList[1024];
char fileNames[1024][32];
mfs_file files[1024];
mfs_dir dirs[1024];
size_t inodeCount = 1;
uint32_t currentInode = 0;

int colour_scheme[] = {7, 0};
int custom_colour_scheme = 0;

void panic(void) {
	for(;;) {}
}

void _main(multiboot_info_t* mbd, unsigned int magic) {
	change_colour(7, 1);
	memcpy(&fileNames[0], "root", strlen("root"));
	dirs[0].size = 0;

	/* Initialize the round-robin scheduler */
	init_scheduler();

	/* Create and admit the kernel system task (pid 0 equivalent) */
	task_t sys_task = task_create(DEFAULT_PRIORITY);
	sys_task.state = TASK_RUNNING;
	admit_task(&sys_task);

	/* Create and admit the shell task (pid 1 equivalent) */
	task_t shell_task = task_create(DEFAULT_PRIORITY);
	admit_task(&shell_task);

	/* Set up the IDT and PIT before enabling interrupts */
	idt_init();
	timer_init(100);  /* 100 Hz tick rate */
	asm volatile ("sti");  /* enable hardware interrupts */

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
	if(magic != MULTIBOOT_BOOTLOADER_MAGIC) {
		printf(KERNEL_PANIC_ERROR);
		printf("Invalid magic number!\r\n");
		panic();
	}

	if(!(mbd->flags >> 6 & 0x1)) {
		printf(KERNEL_PANIC_ERROR);
		printf("Invalid memory map given by GRUB bootloader\r\n");
		panic();
	}
	initialize_memory_manager(mbd->mmap_addr, mbd->mmap_length);

	printf("MEMORY MAP:\r\n");
	int i;
	for(i = 0; i < mbd->mmap_length; i += sizeof(multiboot_memory_map_t)) {
		multiboot_memory_map_t* mmmt = (multiboot_memory_map_t*) (mbd->mmap_addr + i);

		printf("Start addr: \0");
		print_hex(mmmt->addr);
		printf(" | Length: \0");
		print_hex(mmmt->len);
		printf(" | Size: \0");
		print_hex(mmmt->size);
		printf(" | Type : \0");
		if (mmmt->type == MULTIBOOT_MEMORY_AVAILABLE) {
			printf("Available\0");
			initialize_memory_region(mmmt->addr, mmmt->len);
		} else if (mmmt->type == MULTIBOOT_MEMORY_RESERVED) {
			printf("Reserved\0");
			deinitialize_memory_region(mmmt->addr, mmmt->len);
		} else if (mmmt->type == MULTIBOOT_MEMORY_ACPI_RECLAIMABLE) {
			printf("ACPI-Reclaimable\0");
		} else if (mmmt->type == MULTIBOOT_MEMORY_NVS) {
			printf("Non-volatile storage\0");
		} else if (mmmt->type == MULTIBOOT_MEMORY_BADRAM) {
			printf("Faulty RAM\0");
		}
		printf("\r\n\r\n");
	}

	/* Mark the shell task as the running task and enter its loop */
	schedule();
	start_shell();
}

