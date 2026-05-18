/*
 * RTL8139 NIC Driver for MOSS
 * Supports basic send/receive via PCI I/O-mapped registers.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/io.h>
#include <kernel/memory_manager.h>
#include "pci.h"
#include "pic.h"
#include "idt.h"
#include "rtl8139.h"

/* --- Driver State --- */

static int      rtl_active = 0;
static uint16_t rtl_iobase = 0;
static uint8_t  rtl_irq = 0;
static uint8_t  rtl_mac[6];

/* TX buffers: 4 descriptors, each 2048 bytes, physically contiguous */
static uint8_t *tx_buffers[RTL_TX_DESC_COUNT];
static int      tx_cur = 0;  /* Next TX descriptor to use (round-robin) */

/* RX ring buffer */
static uint8_t *rx_buffer = NULL;
static uint32_t rx_offset = 0;  /* Current read offset in the ring buffer */

/* Receive callback */
static rtl8139_rx_callback_t rx_callback = NULL;

/* --- Internal Helpers --- */

static inline void rtl_write8(uint16_t reg, uint8_t val) {
    outb(rtl_iobase + reg, val);
}

static inline void rtl_write16(uint16_t reg, uint16_t val) {
    outw(rtl_iobase + reg, val);
}

static inline void rtl_write32(uint16_t reg, uint32_t val) {
    outl(rtl_iobase + reg, val);
}

static inline uint8_t rtl_read8(uint16_t reg) {
    return inb(rtl_iobase + reg);
}

static inline uint16_t rtl_read16(uint16_t reg) {
    return inw(rtl_iobase + reg);
}

static inline uint32_t rtl_read32(uint16_t reg) {
    return inl(rtl_iobase + reg);
}

/* --- IRQ Handler --- */

static void rtl8139_irq_handler(struct isr_regs *regs) {
    (void)regs;

    uint16_t status = rtl_read16(RTL_ISR);

    if (status == 0)
        return;  /* Not our interrupt */

    /* Acknowledge all pending interrupts */
    rtl_write16(RTL_ISR, status);

    /* Receive OK — process all available packets */
    if (status & INT_ROK) {
        while (!(rtl_read8(RTL_CR) & CR_BUFE)) {
            /* Read the 4-byte header at current offset */
            rtl8139_rx_header_t *hdr =
                (rtl8139_rx_header_t *)(rx_buffer + rx_offset);

            /* Validate the packet */
            if (!(hdr->status & RX_ROK)) {
                /* Bad packet — skip it */
                rx_offset = (rtl_read16(RTL_CBR) + 4) & ~3;
                rx_offset %= RTL_RX_BUF_SIZE;
                continue;
            }

            /* Length includes 4-byte CRC; subtract it for the actual frame */
            uint16_t pkt_len = hdr->length - 4;
            uint8_t *pkt_data = rx_buffer + rx_offset + 4;  /* Skip header */

            /* Deliver to callback */
            if (rx_callback && pkt_len > 0 && pkt_len <= 1518) {
                rx_callback(pkt_data, pkt_len);
            }

            /* Advance read offset: header(4) + length, aligned to 4 bytes */
            rx_offset = (rx_offset + hdr->length + 4 + 3) & ~3;
            rx_offset %= RTL_RX_BUF_SIZE;

            /* Update CAPR (read pointer) — must be offset - 0x10 */
            rtl_write16(RTL_CAPR, (uint16_t)(rx_offset - 16));
        }
    }

    /* Transmit OK/Error — could track stats here */
    if (status & (INT_TOK | INT_TER)) {
        /* Nothing needed for basic operation */
    }

    /* RX overflow — reset the receiver */
    if (status & INT_RXOVW) {
        /* Re-enable receiver */
        rtl_write8(RTL_CR, CR_RE | CR_TE);
    }
}

/* --- Public API --- */

int rtl8139_init(void) {
    /* Find the RTL8139 on the PCI bus */
    pci_device_t *dev = pci_find_device(RTL8139_VENDOR_ID, RTL8139_DEVICE_ID);
    if (!dev) {
        printf("[rtl8139] Device not found on PCI bus\n");
        return -1;
    }

    printf("[rtl8139] Found at PCI %02x:%02x.%d, IRQ %d\n",
           dev->bus, dev->slot, dev->func, dev->irq_line);

    /* Get I/O base from BAR0 (bit 0 indicates I/O space) */
    if (!(dev->bar[0] & 1)) {
        printf("[rtl8139] BAR0 is not I/O mapped — unsupported\n");
        return -1;
    }
    rtl_iobase = (uint16_t)(dev->bar[0] & ~0x3);
    rtl_irq = dev->irq_line;

    printf("[rtl8139] I/O base: 0x%x\n", rtl_iobase);

    /* Enable PCI bus mastering (required for DMA) */
    pci_enable_bus_mastering(dev);

    /* --- Power on the device --- */
    rtl_write8(RTL_CONFIG1, 0x00);

    /* --- Software reset --- */
    rtl_write8(RTL_CR, CR_RST);
    /* Wait for reset to complete (RST bit clears) */
    int timeout = 100000;
    while ((rtl_read8(RTL_CR) & CR_RST) && --timeout > 0)
        ;
    if (timeout <= 0) {
        printf("[rtl8139] Reset timeout\n");
        return -1;
    }

    /* --- Read MAC address --- */
    for (int i = 0; i < 6; i++) {
        rtl_mac[i] = rtl_read8(RTL_IDR0 + i);
    }
    printf("[rtl8139] MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
           rtl_mac[0], rtl_mac[1], rtl_mac[2],
           rtl_mac[3], rtl_mac[4], rtl_mac[5]);

    /* --- Allocate RX buffer (must be physically contiguous) --- */
    /* Need ceil(RTL_RX_BUF_SIZE / 4096) pages */
    uint32_t rx_pages = (RTL_RX_BUF_SIZE + 4095) / 4096;
    rx_buffer = (uint8_t *)allocate_blocks(rx_pages);
    if (!rx_buffer) {
        printf("[rtl8139] Failed to allocate RX buffer\n");
        return -1;
    }
    memset(rx_buffer, 0, RTL_RX_BUF_SIZE);
    rx_offset = 0;

    /* --- Allocate TX buffers (4 descriptors × 2048 bytes each) --- */
    /* Allocate one page per TX buffer (4096 > 2048, so fine) */
    for (int i = 0; i < RTL_TX_DESC_COUNT; i++) {
        tx_buffers[i] = (uint8_t *)allocate_blocks(1);
        if (!tx_buffers[i]) {
            printf("[rtl8139] Failed to allocate TX buffer %d\n", i);
            return -1;
        }
        memset(tx_buffers[i], 0, RTL_TX_BUF_SIZE);
    }
    tx_cur = 0;

    /* --- Configure the receiver --- */
    /* Tell the card where the RX buffer is (physical = virtual, identity-mapped) */
    rtl_write32(RTL_RBSTART, (uint32_t)rx_buffer);

    /* Set RX configuration:
     * - Accept broadcast, multicast, physical match
     * - No WRAP (use ring buffer wrapping logic in software)
     * - Max DMA burst = 256 bytes (bits 10:8 = 010 → no, use 110 = unlimited)
     * - RX buffer length = 8K + 16 (bits 12:11 = 00) */
    rtl_write32(RTL_RCR, RCR_AB | RCR_AM | RCR_APM | RCR_WRAP
                | (6 << 8)   /* Max DMA burst: unlimited */
                | (0 << 11)  /* Buffer size: 8K + 16 */
    );

    /* --- Configure TX ---
     * Set TX start addresses for all 4 descriptors */
    rtl_write32(RTL_TSAD0, (uint32_t)tx_buffers[0]);
    rtl_write32(RTL_TSAD1, (uint32_t)tx_buffers[1]);
    rtl_write32(RTL_TSAD2, (uint32_t)tx_buffers[2]);
    rtl_write32(RTL_TSAD3, (uint32_t)tx_buffers[3]);

    /* TX configuration: IFG = normal, Max DMA burst = 2048 */
    rtl_write32(RTL_TCR, (6 << 8));  /* Max DMA burst = 2048 bytes */

    /* --- Set interrupt mask --- */
    rtl_write16(RTL_IMR, INT_ROK | INT_TOK | INT_RER | INT_TER | INT_RXOVW);

    /* --- Register IRQ handler --- */
    isr_register_handler(32 + rtl_irq, rtl8139_irq_handler);
    pic_clear_mask(rtl_irq);

    /* --- Enable receiver and transmitter --- */
    rtl_write8(RTL_CR, CR_RE | CR_TE);

    /* Accept missed packet counter reset */
    rtl_write32(RTL_MPC, 0);

    rtl_active = 1;
    printf("[rtl8139] Initialized successfully\n");
    return 0;
}

int rtl8139_send(const void *data, uint16_t length) {
    if (!rtl_active)
        return -1;

    if (length > 1518)
        return -1;  /* Too large for Ethernet frame */

    if (length < 60)
        length = 60;  /* Pad to minimum Ethernet frame size */

    /* Copy data into the current TX buffer */
    memcpy(tx_buffers[tx_cur], data, length);

    /* Write the TX status: clear OWN bit (bit 13) and set size.
     * The card DMA's when we write the size with OWN=0. */
    uint16_t tsd_reg = RTL_TSD0 + (tx_cur * 4);

    /* Threshold = 0 (start TX immediately after filling FIFO),
     * Size = length in bits [12:0] */
    rtl_write32(tsd_reg, (uint32_t)length);

    /* Advance to next descriptor */
    tx_cur = (tx_cur + 1) % RTL_TX_DESC_COUNT;

    return 0;
}

const uint8_t *rtl8139_get_mac(void) {
    return rtl_mac;
}

int rtl8139_is_active(void) {
    return rtl_active;
}

void rtl8139_set_rx_callback(rtl8139_rx_callback_t cb) {
    rx_callback = cb;
}
