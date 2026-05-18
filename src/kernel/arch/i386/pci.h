#ifndef ARCH_I386_PCI_H
#define ARCH_I386_PCI_H

#include <stdint.h>

/* PCI Configuration Space I/O ports */
#define PCI_CONFIG_ADDR  0xCF8
#define PCI_CONFIG_DATA  0xCFC

/* PCI configuration space register offsets */
#define PCI_REG_VENDOR_ID    0x00
#define PCI_REG_DEVICE_ID    0x02
#define PCI_REG_COMMAND      0x04
#define PCI_REG_STATUS       0x06
#define PCI_REG_REVISION     0x08
#define PCI_REG_PROG_IF      0x09
#define PCI_REG_SUBCLASS     0x0A
#define PCI_REG_CLASS        0x0B
#define PCI_REG_CACHE_LINE   0x0C
#define PCI_REG_LATENCY      0x0D
#define PCI_REG_HEADER_TYPE  0x0E
#define PCI_REG_BIST         0x0F
#define PCI_REG_BAR0         0x10
#define PCI_REG_BAR1         0x14
#define PCI_REG_BAR2         0x18
#define PCI_REG_BAR3         0x1C
#define PCI_REG_BAR4         0x20
#define PCI_REG_BAR5         0x24
#define PCI_REG_SUBSYS_VENDOR 0x2C
#define PCI_REG_SUBSYS_ID    0x2E
#define PCI_REG_IRQ_LINE     0x3C
#define PCI_REG_IRQ_PIN      0x3D

/* PCI Command register bits */
#define PCI_CMD_IO_SPACE     (1 << 0)
#define PCI_CMD_MEM_SPACE    (1 << 1)
#define PCI_CMD_BUS_MASTER   (1 << 2)
#define PCI_CMD_INT_DISABLE  (1 << 10)

/* Header type flags */
#define PCI_HEADER_MULTIFUNCTION  0x80

/* Maximum devices tracked */
#define PCI_MAX_DEVICES  32

/* PCI device descriptor */
typedef struct {
    uint8_t  bus;
    uint8_t  slot;
    uint8_t  func;
    uint16_t vendor_id;
    uint16_t device_id;
    uint8_t  class_code;
    uint8_t  subclass;
    uint8_t  prog_if;
    uint8_t  revision;
    uint8_t  header_type;
    uint8_t  irq_line;
    uint32_t bar[6];
} pci_device_t;

/* Initialize PCI and enumerate all devices */
void pci_init(void);

/* Read/write PCI configuration space (32-bit aligned) */
uint32_t pci_config_read32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
void     pci_config_write32(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset, uint32_t value);

/* Convenience readers for smaller widths */
uint16_t pci_config_read16(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);
uint8_t  pci_config_read8(uint8_t bus, uint8_t slot, uint8_t func, uint8_t offset);

/* Find a device by vendor/device ID. Returns pointer or NULL. */
pci_device_t *pci_find_device(uint16_t vendor_id, uint16_t device_id);

/* Find a device by class/subclass. Returns pointer or NULL. */
pci_device_t *pci_find_class(uint8_t class_code, uint8_t subclass);

/* Get the number of discovered devices */
int pci_device_count(void);

/* Get device by index (0..count-1) */
pci_device_t *pci_get_device(int index);

/* Enable bus mastering for a device */
void pci_enable_bus_mastering(pci_device_t *dev);

#endif /* ARCH_I386_PCI_H */
