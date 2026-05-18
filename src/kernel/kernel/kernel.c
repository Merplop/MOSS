// MOSS KERNEL
// (C) Miro Haapalainen, 2024
//

#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>
#include <kernel/mouse.h>
#include <kernel/kernel.h>
#include <kernel/memory_manager.h>
#include <kernel/sched.h>
#include <kernel/ext2.h>
#include <kernel/blkdev.h>
#include <kernel/syscall.h>
#include <kernel/users.h>
#include <sys/io.h>
#include <sys/sleep.h>
#include <moss/commands.h>
#include <sys/multiboot.h>

/* Arch-specific interrupt and timer support */
void idt_init(void);
void timer_init(uint32_t frequency);
void paging_init(uint32_t mem_size_kb, uint32_t fb_phys, uint32_t fb_size);

/* TSS / GDT */
void tss_init(uint32_t kernel_ss, uint32_t kernel_esp0);

/* PCI bus enumeration */
void pci_init(void);

/* RTL8139 NIC driver */
int rtl8139_init(void);

/* ATA disk driver */
void ata_init(void);
struct ata_drive;
void *ata_get_drive(int index);
uint32_t ata_drive_sector_count(void *drv);
blkdev_t *ata_get_blkdev(int index);

/* Thin wrapper so kernel.c can read sectors without including ata.h */
int ata_read_sectors_kern(void *drv, uint32_t lba, uint32_t count, void *buf);

/* Ramdisk */
void ramdisk_init(ramdisk_t *rd, void *buf, uint32_t size_bytes);

/* MBR partition table structures */
#define MBR_SIGNATURE    0xAA55
#define MBR_PART_LINUX   0x83
#define MBR_PART_COUNT   4

typedef struct {
    uint8_t  status;
    uint8_t  chs_first[3];
    uint8_t  type;
    uint8_t  chs_last[3];
    uint32_t lba_start;
    uint32_t sector_count;
} __attribute__((packed)) mbr_partition_t;

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
char cwd_path[256] = "/";

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

	/* Initialize serial port for debug logging (mirrors to serial.log) */
	extern void serial_init(void);
	serial_init();

	change_colour(7, 1);

	/* Set up the IDT and PIT before enabling interrupts */
	idt_init();
	timer_init(1000);  /* 1000 Hz tick rate (1 ms per tick) */

	/* Initialize memory manager before anything that calls malloc.
	 * Save the GRUB memory map before placing the bitmap. */
	g_mmap_len = mbd->mmap_length;
	if (g_mmap_len > MMAP_BUF_SIZE)
		g_mmap_len = MMAP_BUF_SIZE;
	memcpy(g_saved_mmap, (void *)mbd->mmap_addr, g_mmap_len);

	/* Calculate total physical memory from the GRUB memory map. */
	extern uint32_t _kernel_end;   /* linker symbol */
	uint32_t total_ram = 0;
	{
		uint32_t i;
		for (i = 0; i < g_mmap_len; i += sizeof(multiboot_memory_map_t)) {
			multiboot_memory_map_t *e =
				(multiboot_memory_map_t *)(g_saved_mmap + i);
			uint32_t region_end = (uint32_t)(e->addr + e->len);
			if (region_end > total_ram)
				total_ram = region_end;
		}
		if (total_ram == 0)
			total_ram = (mbd->mem_upper + 1024) * 1024;
	}

	/* Place the bitmap right after the kernel image (page-aligned). */
	uint32_t bitmap_addr = ((uint32_t)&_kernel_end + 0xFFF) & ~0xFFF;
	initialize_memory_manager(bitmap_addr, total_ram);

	/* Mark available regions as free. */
	{
		uint32_t i;
		for (i = 0; i < g_mmap_len; i += sizeof(multiboot_memory_map_t)) {
			multiboot_memory_map_t *mmmt =
				(multiboot_memory_map_t *)(g_saved_mmap + i);
			if (mmmt->type == MULTIBOOT_MEMORY_AVAILABLE)
				initialize_memory_region((uint32_t)mmmt->addr,
				                         (uint32_t)mmmt->len);
		}
	}

	/* Protect the kernel image + bitmap from being allocated.
	 * Everything from 0 to bitmap_end must be marked as used. */
	{
		uint32_t bitmap_size = total_ram / BLOCK_SIZE / BLOCKS_PER_BYTE;
		uint32_t bitmap_end  = bitmap_addr + bitmap_size;
		uint32_t protect_end = (bitmap_end + BLOCK_SIZE - 1)
		                       & ~(BLOCK_SIZE - 1);
		deinitialize_memory_region(0, protect_end);
	}

	/* Enable paging with identity mapping for all available physical RAM
	 * AND the framebuffer (which lives at a high physical address).
	 * mem_upper is KiB above 1 MiB; add 1024 KiB for conventional memory. */
	uint32_t fb_phys = (uint32_t)mbd->framebuffer_addr;
	uint32_t fb_size = mbd->framebuffer_pitch * mbd->framebuffer_height;
	paging_init(mbd->mem_upper + 1024, fb_phys, fb_size);

	/* Set up the full GDT with ring-3 segments and TSS.
	 * Pass the kernel data segment (0x10) and a temporary ESP0;
	 * the scheduler will update ESP0 on every context switch. */
	{
		uint32_t esp_now;
		asm volatile("movl %%esp, %0" : "=r"(esp_now));
		tss_init(0x10, esp_now);
	}

	/* Initialize the round-robin scheduler (needs malloc → memory manager) */
	init_scheduler();

	/* Initialize syscall infrastructure (int 0x80) */
	syscall_init();

	/* Initialize interrupt-driven PS/2 keyboard (IRQ1) */
	keyboard_init();

	/* Initialize PS/2 mouse (IRQ12) */
	mouse_init();

	/* ---- Filesystem initialisation ----
	 * Priority:
	 *   1. GRUB module → load as ramdisk (works on USB / any hardware)
	 *   2. ATA drives → probe for existing ext2 (works in QEMU)
	 */
	pci_init();  /* enumerate PCI bus — must come before device drivers that use PCI */
	ata_init();  /* always probe ATA so 'disks' command works */

	/* Probe for RTL8139 NIC (PCI) */
	{
		if (rtl8139_init() == 0)
			printf("[kernel] RTL8139 NIC ready\n");
	}

	/* Initialize network stack (ARP/IP/ICMP) */
	{
		extern void net_init(void);
		net_init();
	}

	/* Probe for Sound Blaster 16 (ISA, I/O 0x220, IRQ 5, DMA 1) */
	{
		extern int sb16_init(void);
		if (sb16_init() == 0)
			printf("[kernel] Sound Blaster 16 initialized\n");
	}

	static ramdisk_t g_ramdisk;   /* static so it lives forever */
	int fs_mounted = 0;

	/* Check for a GRUB multiboot module (the ext2 disk image) */
	if ((mbd->flags & MULTIBOOT_INFO_MODS) && mbd->mods_count > 0) {
		multiboot_module_t *mod = (multiboot_module_t *)mbd->mods_addr;
		uint32_t mod_size = mod->mod_end - mod->mod_start;
		printf("GRUB module: %d bytes at 0x%x\r\n", mod_size, mod->mod_start);

		/* Protect module memory from the physical allocator */
		deinitialize_memory_region(mod->mod_start,
		                           (mod_size + BLOCK_SIZE - 1) & ~(BLOCK_SIZE - 1));

		ramdisk_init(&g_ramdisk, (void *)mod->mod_start, mod_size);
		int rc = ext2_init(&g_ramdisk.dev, 0, 1);
		if (rc == 0) {
			printf("ext2 mounted from ramdisk\r\n");
			fs_mounted = 1;
		} else {
			/* Module exists but has no valid ext2 — format it */
			printf("Formatting ramdisk as ext2...\r\n");
			rc = ext2_format(&g_ramdisk.dev, 0, g_ramdisk.dev.sector_count);
			if (rc == 0) {
				printf("ext2 ramdisk ready\r\n");
				fs_mounted = 1;
			}
		}
	}

	/* If no ramdisk, try ATA drives */
	if (!fs_mounted) {
		int found_any = 0;
		for (int d = 0; d < 4; d++) {
			blkdev_t *bdev = ata_get_blkdev(d);
			if (bdev) {
				printf("ATA drive %d: %d MiB\r\n", d,
				       bdev->sector_count / 2048);
				found_any = 1;
			}
		}

		/* Scan drives for an existing MOSS ext2 filesystem.
		 * NEVER auto-format — this protects real hardware drives.
		 * Check both raw (LBA 0) and MBR partitions of type 0x83. */
		for (int d = 0; d < 4 && !fs_mounted; d++) {
			blkdev_t *bdev = ata_get_blkdev(d);
			if (!bdev) continue;

			/* First try raw ext2 at LBA 0 (QEMU / unpartitioned disks) */
			int rc = ext2_init(bdev, 0, 0);
			if (rc == 0) {
				printf("ext2 mounted from ATA drive %d (raw)\r\n", d);
				fs_mounted = 1;
				break;
			}

			/* Check for MBR partition table */
			static uint8_t mbr_buf[512];
			if (bdev->read_sectors(bdev, 0, 1, mbr_buf) != 0)
				continue;

			uint16_t sig = *(uint16_t *)(mbr_buf + 510);
			if (sig != MBR_SIGNATURE)
				continue;

			mbr_partition_t *parts = (mbr_partition_t *)(mbr_buf + 446);
			for (int p = 0; p < MBR_PART_COUNT && !fs_mounted; p++) {
				if (parts[p].type != MBR_PART_LINUX)
					continue;
				if (parts[p].lba_start == 0 || parts[p].sector_count == 0)
					continue;
				rc = ext2_init(bdev, parts[p].lba_start, 0);
				if (rc == 0) {
					printf("ext2 mounted from ATA drive %d partition %d (LBA %d)\r\n",
					       d, p + 1, parts[p].lba_start);
					fs_mounted = 1;
				}
			}
		}
		if (!fs_mounted && found_any) {
			printf("No MOSS ext2 filesystem found.\r\n");
			printf("Use 'disks' to list drives, 'mkfs <n>' to format.\r\n");
		} else if (!found_any && !fs_mounted) {
			printf("No ATA drives found.\r\n");
			printf("If booting from USB, add a GRUB module.\r\n");
		}
	}
	cwd_ino = EXT2_ROOT_INO;

	/* Initialise user management subsystem */
	users_init();

	/* Create the boot/idle task (pid 0) — uses the current stack */
	task_t idle_task = task_create(NULL, DEFAULT_PRIORITY);
	admit_task(&idle_task);
	get_current_task()->state = TASK_RUNNING;

	/* Create the shell task (pid 1) — runs start_shell() on its own stack */
	task_t shell_task = task_create(start_shell, DEFAULT_PRIORITY);
	admit_task(&shell_task);

	/* Print the welcome banner while interrupts are still disabled
	 * so the timer cannot preempt us and switch to the shell before
	 * initialisation is complete. */
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

	/* Switch to the shell task.  Interrupts are still disabled here;
	 * the shell's task_trampoline will call sti once it starts. */
	schedule();

	/* Idle loop — reached when no other tasks need the CPU.
	 * sti + hlt is the standard pattern: sti sets IF but the CPU
	 * defers interrupt delivery until after the next instruction,
	 * so hlt executes atomically before any interrupt can fire. */
	for (;;)
		asm volatile("sti; hlt");
}

