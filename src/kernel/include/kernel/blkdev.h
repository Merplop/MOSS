#ifndef _KERNEL_BLKDEV_H
#define _KERNEL_BLKDEV_H

#include <stdint.h>

/*
 * Generic block device interface.
 *
 * Both the ATA driver and the ramdisk implement this so that the ext2
 * filesystem can work on either backend without knowing which one it
 * is talking to.
 *
 * Sector size is always 512 bytes.
 */

typedef struct blkdev {
    int  (*read_sectors)(struct blkdev *dev, uint32_t lba,
                         uint32_t count, void *buf);
    int  (*write_sectors)(struct blkdev *dev, uint32_t lba,
                          uint32_t count, const void *buf);
    uint32_t sector_count;   /* total sectors on device */
} blkdev_t;

/* ---- Ramdisk ---- */

typedef struct {
    blkdev_t  dev;       /* must be first so we can cast blkdev_t* ↔ ramdisk_t* */
    uint8_t  *data;      /* pointer to the backing memory */
    uint32_t  size;      /* size in bytes */
} ramdisk_t;

/* Initialise a ramdisk over an existing memory buffer.
 * The buffer must remain valid for the lifetime of the ramdisk. */
void ramdisk_init(ramdisk_t *rd, void *buf, uint32_t size_bytes);

#endif /* _KERNEL_BLKDEV_H */
