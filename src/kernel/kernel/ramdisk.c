/*
 * RAM-backed block device.
 *
 * Provides the blkdev_t interface over a contiguous memory buffer so
 * that the ext2 filesystem can be hosted entirely in RAM (e.g. loaded
 * from a GRUB multiboot module).
 *
 * MOSS Kernel
 */

#include <stdint.h>
#include <string.h>
#include <kernel/blkdev.h>

#define SECTOR_SIZE 512

static int ramdisk_read(blkdev_t *dev, uint32_t lba,
                        uint32_t count, void *buf)
{
    ramdisk_t *rd = (ramdisk_t *)dev;
    uint32_t offset = lba * SECTOR_SIZE;
    uint32_t bytes  = count * SECTOR_SIZE;
    if (offset + bytes > rd->size)
        return -1;
    memcpy(buf, rd->data + offset, bytes);
    return 0;
}

static int ramdisk_write(blkdev_t *dev, uint32_t lba,
                         uint32_t count, const void *buf)
{
    ramdisk_t *rd = (ramdisk_t *)dev;
    uint32_t offset = lba * SECTOR_SIZE;
    uint32_t bytes  = count * SECTOR_SIZE;
    if (offset + bytes > rd->size)
        return -1;
    memcpy(rd->data + offset, buf, bytes);
    return 0;
}

void ramdisk_init(ramdisk_t *rd, void *buf, uint32_t size_bytes)
{
    rd->data = (uint8_t *)buf;
    rd->size = size_bytes;
    rd->dev.read_sectors  = ramdisk_read;
    rd->dev.write_sectors = ramdisk_write;
    rd->dev.sector_count  = size_bytes / SECTOR_SIZE;
}
