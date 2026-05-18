#ifndef ARCH_I386_RTL8139_H
#define ARCH_I386_RTL8139_H

#include <stdint.h>

/* RTL8139 PCI identifiers */
#define RTL8139_VENDOR_ID  0x10EC
#define RTL8139_DEVICE_ID  0x8139

/* RTL8139 register offsets (from I/O base) */
#define RTL_IDR0          0x00   /* MAC address bytes 0-3 */
#define RTL_IDR4          0x04   /* MAC address bytes 4-5 */
#define RTL_MAR0          0x08   /* Multicast filter 0-3 */
#define RTL_MAR4          0x0C   /* Multicast filter 4-7 */
#define RTL_TSD0          0x10   /* TX status descriptor 0 */
#define RTL_TSD1          0x14   /* TX status descriptor 1 */
#define RTL_TSD2          0x18   /* TX status descriptor 2 */
#define RTL_TSD3          0x1C   /* TX status descriptor 3 */
#define RTL_TSAD0         0x20   /* TX start address descriptor 0 */
#define RTL_TSAD1         0x24   /* TX start address descriptor 1 */
#define RTL_TSAD2         0x28   /* TX start address descriptor 2 */
#define RTL_TSAD3         0x2C   /* TX start address descriptor 3 */
#define RTL_RBSTART       0x30   /* RX buffer start address */
#define RTL_ERBCR         0x34   /* Early RX byte count */
#define RTL_ERSR          0x36   /* Early RX status */
#define RTL_CR            0x37   /* Command register */
#define RTL_CAPR          0x38   /* Current address of packet read */
#define RTL_CBR           0x3A   /* Current buffer address */
#define RTL_IMR           0x3C   /* Interrupt mask register */
#define RTL_ISR           0x3E   /* Interrupt status register */
#define RTL_TCR           0x40   /* TX configuration register */
#define RTL_RCR           0x44   /* RX configuration register */
#define RTL_TCTR          0x48   /* Timer count register */
#define RTL_MPC           0x4C   /* Missed packet counter */
#define RTL_9346CR        0x50   /* 93C46 command register */
#define RTL_CONFIG0       0x51   /* Configuration register 0 */
#define RTL_CONFIG1       0x52   /* Configuration register 1 */
#define RTL_MSR           0x58   /* Media status register */
#define RTL_BMCR          0x62   /* Basic mode control register */
#define RTL_BMSR          0x64   /* Basic mode status register */

/* Command register bits */
#define CR_RST            0x10   /* Reset */
#define CR_RE             0x08   /* Receiver enable */
#define CR_TE             0x04   /* Transmitter enable */
#define CR_BUFE           0x01   /* Buffer empty */

/* Interrupt status/mask bits */
#define INT_ROK           0x0001 /* Receive OK */
#define INT_RER           0x0002 /* Receive error */
#define INT_TOK           0x0004 /* Transmit OK */
#define INT_TER           0x0008 /* Transmit error */
#define INT_RXOVW         0x0010 /* RX buffer overflow */
#define INT_PUN           0x0020 /* Packet underrun / link change */
#define INT_FOVW          0x0040 /* RX FIFO overflow */
#define INT_TIMEOUT       0x4000 /* Timeout */
#define INT_SERR          0x8000 /* System error */

/* TX status register bits */
#define TSD_OWN           (1 << 13) /* DMA completed */
#define TSD_TUN           (1 << 14) /* TX FIFO underrun */
#define TSD_TOK           (1 << 15) /* TX OK */
#define TSD_SIZE_MASK     0x1FFF    /* Packet size bits [12:0] */

/* RX configuration register bits */
#define RCR_AAP           (1 << 0)  /* Accept all packets */
#define RCR_APM           (1 << 1)  /* Accept physical match */
#define RCR_AM            (1 << 2)  /* Accept multicast */
#define RCR_AB            (1 << 3)  /* Accept broadcast */
#define RCR_WRAP          (1 << 7)  /* Wrap at end of buffer (0 = yes) */

/* RX packet header (prepended to each received frame in the ring buffer) */
typedef struct {
    uint16_t status;
    uint16_t length;   /* Including 4-byte CRC */
} __attribute__((packed)) rtl8139_rx_header_t;

/* RX header status bits */
#define RX_ROK            (1 << 0)
#define RX_FAE            (1 << 1)  /* Frame alignment error */
#define RX_CRC            (1 << 2)  /* CRC error */
#define RX_LONG           (1 << 3)  /* Long packet (>4K) */
#define RX_RUNT           (1 << 4)  /* Runt packet (<64 bytes) */
#define RX_ISE            (1 << 5)  /* Invalid symbol error */
#define RX_BAR            (1 << 13) /* Broadcast address received */
#define RX_PAM            (1 << 14) /* Physical address matched */
#define RX_MAR            (1 << 15) /* Multicast address received */

/* TX buffer size (each descriptor): 2048 bytes is enough for one Ethernet frame */
#define RTL_TX_BUF_SIZE   2048

/* RX buffer: 8K + 16 bytes + 1500 wrap padding */
#define RTL_RX_BUF_SIZE   (8192 + 16 + 1536)

/* Number of TX descriptors */
#define RTL_TX_DESC_COUNT 4

/* Initialize the RTL8139 NIC. Returns 0 on success, -1 if not found. */
int rtl8139_init(void);

/* Send a raw Ethernet frame. Returns 0 on success. */
int rtl8139_send(const void *data, uint16_t length);

/* Get the MAC address (6 bytes). Returns pointer to static buffer. */
const uint8_t *rtl8139_get_mac(void);

/* Check if the NIC is initialized */
int rtl8139_is_active(void);

/* Receive callback type — called from IRQ context for each received packet */
typedef void (*rtl8139_rx_callback_t)(const void *data, uint16_t length);

/* Set the receive callback */
void rtl8139_set_rx_callback(rtl8139_rx_callback_t cb);

#endif /* ARCH_I386_RTL8139_H */
