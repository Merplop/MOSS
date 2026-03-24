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
#include <kernel/ext2.h>
#include <sys/io.h>
#include <sys/sleep.h>
#include <moss/commands.h>
#include <sys/multiboot.h>

/* Arch-specific interrupt and timer support */
void idt_init(void);
void timer_init(uint32_t frequency);
void paging_init(uint32_t mem_size_kb, uint32_t fb_phys, uint32_t fb_size);

/* ATA disk driver */
void ata_init(void);
struct ata_drive;
void *ata_get_drive(int index);

/* Framebuffer support */
typedef struct {
    uint8_t  *address;
    uint32_t  width;
    uint32_t  height;
    uint32_t  pitch;
    uint8_t   bpp;
    uint8_t   red_pos, red_size;
    uint8_t   green_pos, green_size;
    uint8_t   blue_pos, blue_size;
} framebuffer_info_t;
void framebuffer_init(framebuffer_info_t *info);

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
const char DISK_ERROR[] = "ERROR 10 - Disk I/O error\r\n";

const char version[] = "Beta 2.0";
const char welcome1_eng[] = "------------------------------\r\nMOSS Kernel - Version ";
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
int kernel_privilege = 0;

/* --- ext2 current working directory --- */
uint32_t cwd_ino = EXT2_ROOT_INO;

int colour_scheme[] = {7, 0};
int custom_colour_scheme = 0;

/* Saved copy of the GRUB memory map (preserved before the bitmap overwrites it). */
#define MMAP_BUF_SIZE 512
uint8_t  g_saved_mmap[MMAP_BUF_SIZE];
uint32_t g_mmap_len = 0;

void panic(void) {
	for(;;) {}
}

void _main(multiboot_info_t* mbd, unsigned int magic) {
	/* Validate multiboot before anything else.
	 * We cannot print yet (no framebuffer), so just halt on failure. */
	if(magic != MULTIBOOT_BOOTLOADER_MAGIC) {
		panic();
	}

	if(!(mbd->flags >> 6 & 0x1)) {
		panic();
	}

	/* Extract framebuffer info from multiboot.
	 * GRUB should have set a 1920x1080x32 linear framebuffer for us. */
	if(!(mbd->flags & MULTIBOOT_INFO_FRAMEBUFFER_INFO) ||
	    mbd->framebuffer_type != MULTIBOOT_FRAMEBUFFER_TYPE_RGB) {
		panic();
	}

	framebuffer_info_t fb_info;
	fb_info.address   = (uint8_t *)(uint32_t)mbd->framebuffer_addr;
	fb_info.width     = mbd->framebuffer_width;
	fb_info.height    = mbd->framebuffer_height;
	fb_info.pitch     = mbd->framebuffer_pitch;
	fb_info.bpp       = mbd->framebuffer_bpp;
	fb_info.red_pos   = mbd->framebuffer_red_field_position;
	fb_info.red_size  = mbd->framebuffer_red_mask_size;
	fb_info.green_pos = mbd->framebuffer_green_field_position;
	fb_info.green_size= mbd->framebuffer_green_mask_size;
	fb_info.blue_pos  = mbd->framebuffer_blue_field_position;
	fb_info.blue_size = mbd->framebuffer_blue_mask_size;

	/* Initialize framebuffer driver (physical access, no paging yet) */
	framebuffer_init(&fb_info);

	/* Now the terminal works */
	terminal_initialize();
	change_colour(7, 1);

	/* Set up the IDT and PIT before enabling interrupts */
	idt_init();
	timer_init(100);  /* 100 Hz tick rate */

	/* Initialize memory manager before anything that calls malloc.
	 * IMPORTANT: initialize_memory_manager places its bitmap at mmap_addr,
	 * overwriting the GRUB memory map.  Save a copy first. */
	g_mmap_len = mbd->mmap_length;
	if (g_mmap_len > MMAP_BUF_SIZE)
		g_mmap_len = MMAP_BUF_SIZE;
	memcpy(g_saved_mmap, (void *)mbd->mmap_addr, g_mmap_len);

	initialize_memory_manager(mbd->mmap_addr, mbd->mmap_length);

	uint32_t i;
	for(i = 0; i < g_mmap_len; i += sizeof(multiboot_memory_map_t)) {
		multiboot_memory_map_t* mmmt = (multiboot_memory_map_t*) (g_saved_mmap + i);
		if (mmmt->type == MULTIBOOT_MEMORY_AVAILABLE) {
			initialize_memory_region(mmmt->addr, mmmt->len);
		} else if (mmmt->type == MULTIBOOT_MEMORY_RESERVED) {
			deinitialize_memory_region(mmmt->addr, mmmt->len);
		}
	}

	/* Enable paging with identity mapping for all available physical RAM
	 * AND the framebuffer (which lives at a high physical address).
	 * mem_upper is KiB above 1 MiB; add 1024 KiB for conventional memory. */
	uint32_t fb_phys = (uint32_t)mbd->framebuffer_addr;
	uint32_t fb_size = mbd->framebuffer_pitch * mbd->framebuffer_height;
	paging_init(mbd->mem_upper + 1024, fb_phys, fb_size);

	/* Initialize the round-robin scheduler (needs malloc → memory manager) */
	init_scheduler();

	/* Initialize ATA disk driver and ext2 filesystem */
	ata_init();
	{
		/* Try all 4 ATA drive slots to find a usable disk */
		void *disk = 0;
		for (int d = 0; d < 4; d++) {
			disk = ata_get_drive(d);
			if (disk) {
				printf("ATA drive %d detected\r\n", d);
				break;
			}
		}
		if (disk) {
			/* Try LBA 0 (treat the whole disk as the filesystem).
			 * ext2_init will format if no valid superblock is found. */
			int rc = ext2_init(disk, 0, 1);
			if (rc == 0) {
				printf("ext2 mounted OK\r\n");
			} else {
				printf("ext2 init failed (rc=%d)\r\n", rc);
			}
		} else {
			printf("No ATA drive found\r\n");
		}
	}
	cwd_ino = EXT2_ROOT_INO;

	/* Create and admit the kernel system task (pid 0 equivalent) */
	task_t sys_task = task_create(DEFAULT_PRIORITY);
	sys_task.state = TASK_RUNNING;
	admit_task(&sys_task);

	/* Create and admit the shell task (pid 1 equivalent) */
	task_t shell_task = task_create(DEFAULT_PRIORITY);
	admit_task(&shell_task);

	asm volatile ("sti");  /* enable hardware interrupts */

	change_colour(7, 0);
	terminal_initialize();

	printf(welcome1_eng);
	printf(version);
	printf(welcome2);
	if (ext2_get_fs()) {
		printf("ext2 filesystem ready\r\n");
	} else {
		printf("WARNING: No disk found - filesystem unavailable\r\n");
	}

	/* Mark the shell task as the running task and enter its loop */
	schedule();
	start_shell();
}

