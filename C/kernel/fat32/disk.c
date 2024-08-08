#include <disk.h>
#include <string.h>
#include <liballoc.h>

uint16_t mbr_partition_indexes[] = {MBR_PARTITION_1, MBR_PARTITION_2,
				MBR_PARTITION_3, MBR_PARTITION4};

void openDisk(uint32_t disk, uint8_t partition, mbr_partition *out) {
	uint8_t *rawArr = (uint8_t *)malloc(SECTOR_SIZE);
	read_sectors_ATA_PIO(rawArr, 0x0, 1);
	memcpy(out, (void *)((uint32_t)rawArr + mbr_partition_indexes[partition]), sizeof(mbr_partition));
	free(rawArr);
}
