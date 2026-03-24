/*
 * ATA PIO mode disk driver.
 * Supports LBA28 addressing (up to 128 GiB).
 * MOSS Kernel – i386
 */

#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <sys/io.h>
#include "ata.h"

/* Up to 4 drives: primary master/slave, secondary master/slave */
#define MAX_ATA_DRIVES 4
static ata_drive_t drives[MAX_ATA_DRIVES];

/* ------------------------------------------------------------------ */
/*  Interrupt save / restore helpers                                  */
/* ------------------------------------------------------------------ */

static inline uint32_t save_flags_cli(void)
{
    uint32_t flags;
    asm volatile ("pushfl; popl %0; cli" : "=r"(flags) : : "memory");
    return flags;
}

static inline void restore_flags(uint32_t flags)
{
    asm volatile ("pushl %0; popfl" : : "r"(flags) : "memory", "cc");
}

/* ------------------------------------------------------------------ */
/*  Internal helpers                                                  */
/* ------------------------------------------------------------------ */

static void ata_400ns_delay(uint16_t ctrl_base)
{
    /* Reading the alternate status port wastes ~100 ns each time */
    inb(ctrl_base + ATA_REG_ALTSTATUS);
    inb(ctrl_base + ATA_REG_ALTSTATUS);
    inb(ctrl_base + ATA_REG_ALTSTATUS);
    inb(ctrl_base + ATA_REG_ALTSTATUS);
}

/* Check whether the bus is floating (no device connected). */
static int ata_is_floating(uint16_t io_base)
{
    uint8_t s = inb(io_base + ATA_REG_STATUS);
    return (s == 0xFF);
}

static int ata_wait_bsy(uint16_t io_base)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(io_base + ATA_REG_STATUS);
        if (status == 0xFF)
            return -1;  /* floating bus – no device */
        if (!(status & ATA_SR_BSY))
            return 0;
    }
    return -1;  /* timeout */
}

static int ata_wait_drq(uint16_t io_base)
{
    for (int i = 0; i < 100000; i++) {
        uint8_t status = inb(io_base + ATA_REG_STATUS);
        if (status == 0xFF)
            return -1;  /* floating bus */
        if (status & ATA_SR_ERR)
            return -1;
        if (status & ATA_SR_DF)
            return -1;
        if (status & ATA_SR_DRQ)
            return 0;
    }
    return -1;  /* timeout */
}

static int ata_identify(uint16_t io_base, uint16_t ctrl_base,
                        uint8_t drive_sel, uint16_t *id_buf)
{
    /* Check for floating bus before doing anything */
    if (ata_is_floating(io_base))
        return -1;

    /* Software reset the bus */
    outb(ctrl_base + ATA_REG_CTRL, 0x04);
    io_wait();
    io_wait();
    outb(ctrl_base + ATA_REG_CTRL, 0x00);
    io_wait();
    io_wait();

    /* Wait for the bus to settle after reset */
    ata_400ns_delay(ctrl_base);
    ata_wait_bsy(io_base);

    /* Select drive */
    outb(io_base + ATA_REG_DRIVE, drive_sel);
    ata_400ns_delay(ctrl_base);

    /* Check floating bus again after drive select */
    if (ata_is_floating(io_base))
        return -1;

    /* Zero out sector count / LBA registers */
    outb(io_base + ATA_REG_SECCOUNT, 0);
    outb(io_base + ATA_REG_LBA_LO, 0);
    outb(io_base + ATA_REG_LBA_MID, 0);
    outb(io_base + ATA_REG_LBA_HI, 0);

    /* Send IDENTIFY */
    outb(io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    ata_400ns_delay(ctrl_base);

    uint8_t status = inb(io_base + ATA_REG_STATUS);
    if (status == 0 || status == 0xFF)
        return -1;  /* no drive or floating bus */

    /* Wait for BSY to clear */
    if (ata_wait_bsy(io_base) != 0)
        return -1;

    /* Check if this is an ATAPI/SATA device (not plain ATA) */
    uint8_t lba_mid = inb(io_base + ATA_REG_LBA_MID);
    uint8_t lba_hi  = inb(io_base + ATA_REG_LBA_HI);
    if (lba_mid != 0 || lba_hi != 0)
        return -1;  /* ATAPI or SATA – not a plain ATA disk */

    /* Wait for DRQ or ERR */
    if (ata_wait_drq(io_base) != 0)
        return -1;

    /* Read 256 words of identification data */
    insw(io_base + ATA_REG_DATA, id_buf, 256);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Public API                                                        */
/* ------------------------------------------------------------------ */

void ata_init(void)
{
    memset(drives, 0, sizeof(drives));

    static const struct {
        uint16_t io_base;
        uint16_t ctrl_base;
        uint8_t  drive_sel;
    } probes[MAX_ATA_DRIVES] = {
        { ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   ATA_DRIVE_MASTER },
        { ATA_PRIMARY_IO,   ATA_PRIMARY_CTRL,   ATA_DRIVE_SLAVE  },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, ATA_DRIVE_MASTER },
        { ATA_SECONDARY_IO, ATA_SECONDARY_CTRL, ATA_DRIVE_SLAVE  },
    };

    uint16_t id_buf[256];

    for (int i = 0; i < MAX_ATA_DRIVES; i++) {
        drives[i].io_base      = probes[i].io_base;
        drives[i].ctrl_base    = probes[i].ctrl_base;
        drives[i].drive_select = probes[i].drive_sel;
        drives[i].present      = 0;

        if (ata_identify(probes[i].io_base, probes[i].ctrl_base,
                         probes[i].drive_sel, id_buf) == 0) {
            drives[i].present = 1;
            /* Words 60-61: total addressable LBA28 sectors */
            drives[i].sector_count =
                (uint32_t)id_buf[60] | ((uint32_t)id_buf[61] << 16);
        }
    }
}

ata_drive_t *ata_get_drive(int index)
{
    if (index < 0 || index >= MAX_ATA_DRIVES)
        return 0;
    if (!drives[index].present)
        return 0;
    return &drives[index];
}

int ata_read_sectors(ata_drive_t *drive, uint32_t lba, uint32_t count,
                     void *buf)
{
    if (!drive || !drive->present || count == 0)
        return -1;

    uint8_t *dst = (uint8_t *)buf;
    uint32_t flags = save_flags_cli();

    while (count > 0) {
        /* LBA28 supports max 256 sectors per command (0 means 256) */
        uint32_t batch = (count > 256) ? 256 : count;
        uint8_t  sc    = (batch == 256) ? 0 : (uint8_t)batch;

        if (ata_wait_bsy(drive->io_base) != 0) {
            restore_flags(flags);
            return -1;
        }

        outb(drive->io_base + ATA_REG_DRIVE,
             drive->drive_select | ((lba >> 24) & 0x0F));
        outb(drive->io_base + ATA_REG_SECCOUNT, sc);
        outb(drive->io_base + ATA_REG_LBA_LO,  (uint8_t)(lba));
        outb(drive->io_base + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
        outb(drive->io_base + ATA_REG_LBA_HI,  (uint8_t)(lba >> 16));
        outb(drive->io_base + ATA_REG_COMMAND,  ATA_CMD_READ_PIO);

        for (uint32_t s = 0; s < batch; s++) {
            ata_400ns_delay(drive->ctrl_base);
            if (ata_wait_drq(drive->io_base) != 0) {
                restore_flags(flags);
                return -1;
            }
            insw(drive->io_base + ATA_REG_DATA, dst, 256);
            dst += ATA_SECTOR_SIZE;
        }

        lba   += batch;
        count -= batch;
    }
    restore_flags(flags);
    return 0;
}

int ata_write_sectors(ata_drive_t *drive, uint32_t lba, uint32_t count,
                      const void *buf)
{
    if (!drive || !drive->present || count == 0)
        return -1;

    const uint8_t *src = (const uint8_t *)buf;
    uint32_t flags = save_flags_cli();

    while (count > 0) {
        uint32_t batch = (count > 256) ? 256 : count;
        uint8_t  sc    = (batch == 256) ? 0 : (uint8_t)batch;

        if (ata_wait_bsy(drive->io_base) != 0) {
            restore_flags(flags);
            return -1;
        }

        outb(drive->io_base + ATA_REG_DRIVE,
             drive->drive_select | ((lba >> 24) & 0x0F));
        outb(drive->io_base + ATA_REG_SECCOUNT, sc);
        outb(drive->io_base + ATA_REG_LBA_LO,  (uint8_t)(lba));
        outb(drive->io_base + ATA_REG_LBA_MID, (uint8_t)(lba >> 8));
        outb(drive->io_base + ATA_REG_LBA_HI,  (uint8_t)(lba >> 16));
        outb(drive->io_base + ATA_REG_COMMAND,  ATA_CMD_WRITE_PIO);

        for (uint32_t s = 0; s < batch; s++) {
            ata_400ns_delay(drive->ctrl_base);
            if (ata_wait_drq(drive->io_base) != 0) {
                restore_flags(flags);
                return -1;
            }
            outsw(drive->io_base + ATA_REG_DATA, src, 256);
            src += ATA_SECTOR_SIZE;
        }

        lba   += batch;
        count -= batch;
    }
    restore_flags(flags);
    return ata_flush(drive);
}

int ata_flush(ata_drive_t *drive)
{
    if (!drive || !drive->present)
        return -1;

    uint32_t flags = save_flags_cli();
    outb(drive->io_base + ATA_REG_DRIVE, drive->drive_select);
    outb(drive->io_base + ATA_REG_COMMAND, ATA_CMD_FLUSH);
    ata_400ns_delay(drive->ctrl_base);
    int ret = ata_wait_bsy(drive->io_base);
    restore_flags(flags);
    return ret;
}
