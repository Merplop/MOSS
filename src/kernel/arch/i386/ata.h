#ifndef ARCH_I386_ATA_H
#define ARCH_I386_ATA_H

#include <stdint.h>

/* ATA PIO I/O port offsets (relative to base) */
#define ATA_REG_DATA       0x00
#define ATA_REG_ERROR      0x01
#define ATA_REG_FEATURES   0x01
#define ATA_REG_SECCOUNT   0x02
#define ATA_REG_LBA_LO     0x03
#define ATA_REG_LBA_MID    0x04
#define ATA_REG_LBA_HI     0x05
#define ATA_REG_DRIVE      0x06
#define ATA_REG_STATUS     0x07
#define ATA_REG_COMMAND    0x07

/* ATA control register offset (relative to ctrl base) */
#define ATA_REG_CTRL       0x00
#define ATA_REG_ALTSTATUS  0x00

/* ATA status bits */
#define ATA_SR_BSY   0x80
#define ATA_SR_DRDY  0x40
#define ATA_SR_DF    0x20
#define ATA_SR_DSC   0x10
#define ATA_SR_DRQ   0x08
#define ATA_SR_CORR  0x04
#define ATA_SR_IDX   0x02
#define ATA_SR_ERR   0x01

/* ATA commands */
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30
#define ATA_CMD_FLUSH       0xE7
#define ATA_CMD_IDENTIFY     0xEC

/* Drive select values for the DRIVE register */
#define ATA_DRIVE_MASTER  0xE0
#define ATA_DRIVE_SLAVE   0xF0

/* Standard ATA I/O bases */
#define ATA_PRIMARY_IO     0x1F0
#define ATA_PRIMARY_CTRL   0x3F6
#define ATA_SECONDARY_IO   0x170
#define ATA_SECONDARY_CTRL 0x376

/* Sector size */
#define ATA_SECTOR_SIZE 512

typedef struct {
    uint16_t io_base;
    uint16_t ctrl_base;
    uint8_t  drive_select;   /* ATA_DRIVE_MASTER or ATA_DRIVE_SLAVE */
    uint8_t  present;        /* 1 if drive was detected */
    uint32_t sector_count;   /* total LBA28 sector count */
} ata_drive_t;

/* Initialise ATA subsystem, detect drives. */
void ata_init(void);

/* Read `count` sectors starting at `lba` into `buf`.
 * Returns 0 on success, -1 on error. */
int ata_read_sectors(ata_drive_t *drive, uint32_t lba, uint32_t count,
                     void *buf);

/* Write `count` sectors starting at `lba` from `buf`.
 * Returns 0 on success, -1 on error. */
int ata_write_sectors(ata_drive_t *drive, uint32_t lba, uint32_t count,
                      const void *buf);

/* Flush the drive's write cache. */
int ata_flush(ata_drive_t *drive);

/* Get the first detected ATA drive (or NULL). */
ata_drive_t *ata_get_drive(int index);

#endif /* ARCH_I386_ATA_H */
