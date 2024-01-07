#include<disk.h>
#include<fat32.h>
#include<liballoc.h>
#include<system.h>

uint32_t getFatEntry(uint32_t cluster) {
	uint32_t lba = fat->fat_begin_lba + (cluster*4/SECTOR_SIZE);

}
