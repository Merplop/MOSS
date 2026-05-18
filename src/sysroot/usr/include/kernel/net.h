#ifndef KERNEL_NET_H
#define KERNEL_NET_H

#include <stdint.h>

/* --- Ethernet --- */

#define ETH_ALEN       6       /* MAC address length */
#define ETH_HLEN       14      /* Ethernet header length */
#define ETH_MTU        1500    /* Maximum payload */
#define ETH_FRAME_MAX  1518    /* Header + payload + CRC */

#define ETH_TYPE_ARP   0x0806
#define ETH_TYPE_IP    0x0800

typedef struct {
    uint8_t  dst[ETH_ALEN];
    uint8_t  src[ETH_ALEN];
    uint16_t ethertype;        /* Big-endian */
} __attribute__((packed)) eth_header_t;

/* --- ARP --- */

#define ARP_HW_ETHER   1
#define ARP_OP_REQUEST  1
#define ARP_OP_REPLY    2

typedef struct {
    uint16_t hw_type;          /* Hardware type (1 = Ethernet) */
    uint16_t proto_type;       /* Protocol type (0x0800 = IPv4) */
    uint8_t  hw_len;           /* Hardware address length (6) */
    uint8_t  proto_len;        /* Protocol address length (4) */
    uint16_t opcode;           /* 1 = request, 2 = reply */
    uint8_t  sender_mac[ETH_ALEN];
    uint8_t  sender_ip[4];
    uint8_t  target_mac[ETH_ALEN];
    uint8_t  target_ip[4];
} __attribute__((packed)) arp_header_t;

/* --- IPv4 --- */

#define IP_PROTO_ICMP  1
#define IP_PROTO_TCP   6
#define IP_PROTO_UDP   17

typedef struct {
    uint8_t  version_ihl;      /* Version (4 bits) + IHL (4 bits) */
    uint8_t  tos;
    uint16_t total_length;     /* Big-endian */
    uint16_t identification;
    uint16_t flags_fragment;   /* Flags (3 bits) + Fragment offset (13 bits) */
    uint8_t  ttl;
    uint8_t  protocol;
    uint16_t checksum;
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
} __attribute__((packed)) ip_header_t;

/* --- ICMP --- */

#define ICMP_ECHO_REPLY    0
#define ICMP_ECHO_REQUEST  8

typedef struct {
    uint8_t  type;
    uint8_t  code;
    uint16_t checksum;
    uint16_t identifier;
    uint16_t sequence;
} __attribute__((packed)) icmp_header_t;

/* --- UDP --- */

typedef struct {
    uint16_t src_port;         /* Big-endian */
    uint16_t dst_port;         /* Big-endian */
    uint16_t length;           /* Header + data, big-endian */
    uint16_t checksum;         /* Optional in IPv4 (can be 0) */
} __attribute__((packed)) udp_header_t;

/* --- DNS --- */

#define DNS_PORT           53
#define DNS_MAX_NAME       256
#define DNS_FLAG_QR        0x8000  /* Response flag */
#define DNS_FLAG_RD        0x0100  /* Recursion desired */
#define DNS_FLAG_RA        0x0080  /* Recursion available */
#define DNS_TYPE_A         1       /* IPv4 address */
#define DNS_CLASS_IN       1       /* Internet */

typedef struct {
    uint16_t id;
    uint16_t flags;
    uint16_t qdcount;          /* Number of questions */
    uint16_t ancount;          /* Number of answers */
    uint16_t nscount;          /* Number of authority records */
    uint16_t arcount;          /* Number of additional records */
} __attribute__((packed)) dns_header_t;

/* --- Network Configuration --- */

typedef struct {
    uint8_t  ip[4];
    uint8_t  subnet[4];
    uint8_t  gateway[4];
    uint8_t  mac[ETH_ALEN];
    int      configured;
} net_config_t;

/* --- ARP Table --- */

#define ARP_TABLE_SIZE 16

typedef struct {
    uint8_t  ip[4];
    uint8_t  mac[ETH_ALEN];
    uint8_t  valid;
} arp_entry_t;

/* --- Public API --- */

/* Initialize the network stack (call after rtl8139_init) */
void net_init(void);

/* Configure IP address (static) */
void net_configure(const uint8_t ip[4], const uint8_t subnet[4], const uint8_t gateway[4]);

/* Send an ICMP echo request (ping). Returns 0 on success (reply received). */
int net_ping(const uint8_t dst_ip[4], uint16_t seq, uint32_t timeout_ms);

/* Get current network config */
const net_config_t *net_get_config(void);

/* Shell command */
void ping_cmd(void);
void resolve_cmd(void);

/* --- UDP API --- */

/* Send a UDP datagram. Returns 0 on success. */
int udp_send(const uint8_t dst_ip[4], uint16_t src_port, uint16_t dst_port,
             const void *data, uint16_t data_len);

/* Register a UDP receive handler for a specific port. Only one handler per port.
 * Callback receives: source IP, source port, data pointer, data length. */
typedef void (*udp_rx_handler_t)(const uint8_t src_ip[4], uint16_t src_port,
                                 const void *data, uint16_t len);
void udp_register_handler(uint16_t port, udp_rx_handler_t handler);

/* --- DNS API --- */

/* Resolve a hostname to an IPv4 address. Returns 0 on success, -1 on failure.
 * Uses QEMU's built-in DNS server at 10.0.2.3. */
int dns_resolve(const char *hostname, uint8_t out_ip[4]);

/* --- TCP --- */

typedef struct {
    uint16_t src_port;
    uint16_t dst_port;
    uint32_t seq_num;
    uint32_t ack_num;
    uint8_t  data_offset;      /* Upper 4 bits = header length in 32-bit words */
    uint8_t  flags;
    uint16_t window;
    uint16_t checksum;
    uint16_t urgent_ptr;
} __attribute__((packed)) tcp_header_t;

/* TCP flag bits */
#define TCP_FIN  0x01
#define TCP_SYN  0x02
#define TCP_RST  0x04
#define TCP_PSH  0x08
#define TCP_ACK  0x10
#define TCP_URG  0x20

/* TCP connection states */
#define TCP_STATE_CLOSED      0
#define TCP_STATE_SYN_SENT    1
#define TCP_STATE_ESTABLISHED 2
#define TCP_STATE_FIN_WAIT_1  3
#define TCP_STATE_FIN_WAIT_2  4
#define TCP_STATE_CLOSE_WAIT  5
#define TCP_STATE_LAST_ACK    6
#define TCP_STATE_TIME_WAIT   7

/* TCP connection handle (opaque index) */
typedef int tcp_conn_t;

#define TCP_MAX_CONNECTIONS 8
#define TCP_RX_BUF_SIZE    16384
#define TCP_TX_BUF_SIZE    4096

/* --- TCP API --- */

/* Open a TCP connection (active open). Returns connection handle or -1. */
tcp_conn_t tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port, uint32_t timeout_ms);

/* Send data on an established connection. Returns bytes sent or -1. */
int tcp_send(tcp_conn_t conn, const void *data, uint16_t len);

/* Receive data from an established connection. Returns bytes read or -1.
 * Blocks until data available or timeout (0 = wait forever). */
int tcp_recv(tcp_conn_t conn, void *buf, uint16_t max_len, uint32_t timeout_ms);

/* Close a TCP connection gracefully. */
void tcp_close(tcp_conn_t conn);

/* Check if a connection is still established. */
int tcp_is_connected(tcp_conn_t conn);

/* Called from IP layer to dispatch incoming TCP segments */
void tcp_handle_segment(const uint8_t *src_ip, const uint8_t *payload, uint16_t len);

/* Shell command */
void wget_cmd(void);

/* --- Byte-order helpers --- */

static inline uint16_t htons(uint16_t x) {
    return (x >> 8) | (x << 8);
}

static inline uint16_t ntohs(uint16_t x) {
    return htons(x);
}

static inline uint32_t htonl(uint32_t x) {
    return ((x >> 24) & 0xFF)
         | ((x >>  8) & 0xFF00)
         | ((x <<  8) & 0xFF0000)
         | ((x << 24) & 0xFF000000);
}

static inline uint32_t ntohl(uint32_t x) {
    return htonl(x);
}

#endif /* KERNEL_NET_H */
