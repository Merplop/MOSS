/*
 * PCI Bus Enumeration for MOSS
 * Scans all PCI buses and enumerates devices using Configuration Space Access Mechanism #1
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/io.h>
#include "pci.h"

/* Discovered devices */
static pci_device_t devices[PCI_MAX_DEVICES];
static int num_devices = 0;

/* --- Configuration Space Access --- */

uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t address = (1U << 31)          /* Enable bit */
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)slot << 11)
                     | ((uint32_t)func << 8)
                     | (offset & 0xFC);     /* Align to 32-bit */
    outl(PCI_CONFIG_ADDR, address);
    return inl(PCI_CONFIG_DATA);
}

void pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value) {
    uint32_t address = (1U << 31)
                     | ((uint32_t)bus << 16)
                     | ((uint32_t)slot << 11)
                     | ((uint32_t)func << 8)
                     | (offset & 0xFC);
    outl(PCI_CONFIG_ADDR, address);
    outl(PCI_CONFIG_DATA, value);
}

uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read32(bus, slot, func, offset);
    return (uint16_t)(val >> ((offset & 2) * 8));
}

uint8_t pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset) {
    uint32_t val = pci_config_read32(bus, slot, func, offset);
    return (uint8_t)(val >> ((offset & 3) * 8));
}

/* --- Device Scanning --- */

static void pci_scan_function(uint8_t bus, uint8_t slot, uint8_t func) {
    uint32_t reg0 = pci_config_read32(bus, slot, func, 0x00);
    uint16_t vendor = reg0 & 0xFFFF;
    uint16_t device = reg0 >> 16;

    if (vendor == 0xFFFF)
        return;  /* No device */

    if (num_devices >= PCI_MAX_DEVICES)
        return;

    pci_device_t *dev = &devices[num_devices];
    dev->bus       = bus;
    dev->slot      = slot;
    dev->func      = func;
    dev->vendor_id = vendor;
    dev->device_id = device;

    uint32_t reg2 = pci_config_read32(bus, slot, func, 0x08);
    dev->revision   = reg2 & 0xFF;
    dev->prog_if    = (reg2 >> 8) & 0xFF;
    dev->subclass   = (reg2 >> 16) & 0xFF;
    dev->class_code = (reg2 >> 24) & 0xFF;

    dev->header_type = pci_config_read8(bus, slot, func, PCI_REG_HEADER_TYPE);
    dev->irq_line    = pci_config_read8(bus, slot, func, PCI_REG_IRQ_LINE);

    /* Read BARs (only for standard header type 0x00) */
    if ((dev->header_type & 0x7F) == 0x00) {
        for (int i = 0; i < 6; i++) {
            dev->bar[i] = pci_config_read32(bus, slot, func, PCI_REG_BAR0 + i * 4);
        }
    } else {
        memset(dev->bar, 0, sizeof(dev->bar));
    }

    num_devices++;
}

static void pci_scan_slot(uint8_t bus, uint8_t slot) {
    uint32_t reg0 = pci_config_read32(bus, slot, 0, 0x00);
    uint16_t vendor = reg0 & 0xFFFF;

    if (vendor == 0xFFFF)
        return;  /* Empty slot */

    pci_scan_function(bus, slot, 0);

    /* Check if multifunction device */
    uint8_t header = pci_config_read8(bus, slot, 0, PCI_REG_HEADER_TYPE);
    if (header & PCI_HEADER_MULTIFUNCTION) {
        for (uint8_t func = 1; func < 8; func++) {
            pci_scan_function(bus, slot, func);
        }
    }
}

static void pci_scan_bus(uint8_t bus) {
    for (uint8_t slot = 0; slot < 32; slot++) {
        pci_scan_slot(bus, slot);
    }
}

/* --- Public API --- */

void pci_init(void) {
    num_devices = 0;

    /* Check if the host bridge (bus 0, slot 0) is multifunction to determine
     * if there are multiple PCI buses behind separate host bridges. */
    uint8_t header = pci_config_read8(0, 0, 0, PCI_REG_HEADER_TYPE);

    if (!(header & PCI_HEADER_MULTIFUNCTION)) {
        /* Single PCI host — scan bus 0 only (covers most QEMU configs) */
        pci_scan_bus(0);
    } else {
        /* Multiple host bridges — check functions 0..7 of device 0 for valid buses */
        for (uint8_t func = 0; func < 8; func++) {
            uint32_t reg0 = pci_config_read32(0, 0, func, 0x00);
            if ((reg0 & 0xFFFF) == 0xFFFF)
                break;
            pci_scan_bus(func);
        }
    }

    printf("[pci] Found %d device(s):\n", num_devices);
    for (int i = 0; i < num_devices; i++) {
        pci_device_t *d = &devices[i];
        printf("  %02x:%02x.%d  %04x:%04x  class %02x:%02x  IRQ %d\n",
               d->bus, d->slot, d->func,
               d->vendor_id, d->device_id,
               d->class_code, d->subclass,
               d->irq_line);
    }
}

pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id) {
    for (int i = 0; i < num_devices; i++) {
        if (devices[i].vendor_id == vendor_id && devices[i].device_id == device_id)
            return &devices[i];
    }
    return NULL;
}

pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass) {
    for (int i = 0; i < num_devices; i++) {
        if (devices[i].class_code == class_code && devices[i].subclass == subclass)
            return &devices[i];
    }
    return NULL;
}

int pci_device_count(void) {
    return num_devices;
}

pci_device_t *pci_get_device(int index) {
    if (index < 0 || index >= num_devices)
        return NULL;
    return &devices[index];
}

void pci_enable_bus_mastering(pci_device_t *dev) {
    uint32_t cmd = pci_config_read32(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND);
    cmd |= PCI_CMD_BUS_MASTER;
    pci_config_write32(dev->bus, dev->slot, dev->func, PCI_REG_COMMAND, cmd);
}
