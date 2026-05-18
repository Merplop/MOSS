/*
 * MOSS Network Stack
 * Implements Ethernet, ARP, IPv4, and ICMP (ping).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/net.h>
#include <kernel/kernel.h>

/* RTL8139 driver interface */
extern int  rtl8139_send(const void *data, uint16_t length);
extern const uint8_t *rtl8139_get_mac(void);
extern int  rtl8139_is_active(void);
extern void rtl8139_set_rx_callback(void (*cb)(const void *data, uint16_t length));

/* Timer */
extern uint32_t timer_get_ticks(void);

/* Shell args */
extern int   argc;
extern char *argv[];

/* --- State --- */

net_config_t config;
static arp_entry_t  arp_table[ARP_TABLE_SIZE];

/* Ping state (simple: one outstanding ping at a time) */
static volatile int    ping_reply_received = 0;
static volatile uint32_t ping_reply_time = 0;
static uint16_t        ping_expected_seq = 0;
static uint16_t        ping_expected_id = 0;

/* Packet ID counter */
static uint16_t ip_id_counter = 1;

/* Broadcast MAC */
static const uint8_t MAC_BROADCAST[ETH_ALEN] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* --- UDP Port Handlers --- */

#define UDP_MAX_HANDLERS 8

typedef struct {
    uint16_t port;
    udp_rx_handler_t handler;
} udp_port_handler_t;

static udp_port_handler_t udp_handlers[UDP_MAX_HANDLERS];
static int udp_handler_count = 0;

/* DNS state */
static volatile int    dns_reply_received = 0;
static uint8_t         dns_resolved_ip[4];
static uint16_t        dns_transaction_id = 0;

/* --- Utility --- */

uint16_t ip_checksum(const void *data, uint16_t length) {
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;

    while (length > 1) {
        sum += *ptr++;
        length -= 2;
    }
    if (length == 1) {
        sum += *(const uint8_t *)ptr;
    }
    while (sum >> 16)
        sum = (sum & 0xFFFF) + (sum >> 16);

    return (uint16_t)~sum;
}

static int ip_eq(const uint8_t a[4], const uint8_t b[4]) {
    return memcmp(a, b, 4) == 0;
}

static void ip_copy(uint8_t dst[4], const uint8_t src[4]) {
    memcpy(dst, src, 4);
}

/* --- ARP Table --- */

static arp_entry_t *arp_lookup(const uint8_t ip[4]) {
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && ip_eq(arp_table[i].ip, ip))
            return &arp_table[i];
    }
    return NULL;
}

static void arp_table_add(const uint8_t ip[4], const uint8_t mac[ETH_ALEN]) {
    /* Update existing entry */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (arp_table[i].valid && ip_eq(arp_table[i].ip, ip)) {
            memcpy(arp_table[i].mac, mac, ETH_ALEN);
            return;
        }
    }
    /* Find empty slot */
    for (int i = 0; i < ARP_TABLE_SIZE; i++) {
        if (!arp_table[i].valid) {
            ip_copy(arp_table[i].ip, ip);
            memcpy(arp_table[i].mac, mac, ETH_ALEN);
            arp_table[i].valid = 1;
            return;
        }
    }
    /* Table full — overwrite slot 0 */
    ip_copy(arp_table[0].ip, ip);
    memcpy(arp_table[0].mac, mac, ETH_ALEN);
    arp_table[0].valid = 1;
}

/* --- Ethernet --- */

static int eth_send(const uint8_t dst_mac[ETH_ALEN], uint16_t ethertype,
                    const void *payload, uint16_t payload_len) {
    uint8_t frame[ETH_FRAME_MAX];
    if (payload_len + ETH_HLEN > ETH_FRAME_MAX)
        return -1;

    eth_header_t *eth = (eth_header_t *)frame;
    memcpy(eth->dst, dst_mac, ETH_ALEN);
    memcpy(eth->src, config.mac, ETH_ALEN);
    eth->ethertype = htons(ethertype);

    memcpy(frame + ETH_HLEN, payload, payload_len);

    return rtl8139_send(frame, ETH_HLEN + payload_len);
}

/* --- ARP --- */

static void arp_send_request(const uint8_t target_ip[4]) {
    arp_header_t arp;
    arp.hw_type    = htons(ARP_HW_ETHER);
    arp.proto_type = htons(ETH_TYPE_IP);
    arp.hw_len     = ETH_ALEN;
    arp.proto_len  = 4;
    arp.opcode     = htons(ARP_OP_REQUEST);

    memcpy(arp.sender_mac, config.mac, ETH_ALEN);
    ip_copy(arp.sender_ip, config.ip);
    memset(arp.target_mac, 0, ETH_ALEN);
    ip_copy(arp.target_ip, target_ip);

    eth_send(MAC_BROADCAST, ETH_TYPE_ARP, &arp, sizeof(arp));
}

static void arp_send_reply(const uint8_t dst_mac[ETH_ALEN], const uint8_t dst_ip[4]) {
    arp_header_t arp;
    arp.hw_type    = htons(ARP_HW_ETHER);
    arp.proto_type = htons(ETH_TYPE_IP);
    arp.hw_len     = ETH_ALEN;
    arp.proto_len  = 4;
    arp.opcode     = htons(ARP_OP_REPLY);

    memcpy(arp.sender_mac, config.mac, ETH_ALEN);
    ip_copy(arp.sender_ip, config.ip);
    memcpy(arp.target_mac, dst_mac, ETH_ALEN);
    ip_copy(arp.target_ip, dst_ip);

    eth_send(dst_mac, ETH_TYPE_ARP, &arp, sizeof(arp));
}

static void arp_handle(const arp_header_t *arp) {
    uint16_t op = ntohs(arp->opcode);

    /* Learn sender's MAC regardless of opcode */
    arp_table_add(arp->sender_ip, arp->sender_mac);

    if (op == ARP_OP_REQUEST) {
        /* Is the request for our IP? */
        if (ip_eq(arp->target_ip, config.ip)) {
            arp_send_reply(arp->sender_mac, arp->sender_ip);
        }
    }
    /* ARP_OP_REPLY: already learned above */
}

/* --- IPv4 --- */

int ip_send(const uint8_t dst_ip[4], uint8_t protocol,
                   const void *payload, uint16_t payload_len) {
    uint8_t pkt[ETH_MTU];
    uint16_t total_len = sizeof(ip_header_t) + payload_len;

    if (total_len > ETH_MTU)
        return -1;

    ip_header_t *ip = (ip_header_t *)pkt;
    ip->version_ihl   = 0x45;  /* IPv4, 5 dwords (20 bytes) */
    ip->tos           = 0;
    ip->total_length  = htons(total_len);
    ip->identification = htons(ip_id_counter++);
    ip->flags_fragment = htons(0x4000);  /* Don't fragment */
    ip->ttl           = 64;
    ip->protocol      = protocol;
    ip->checksum      = 0;
    ip_copy(ip->src_ip, config.ip);
    ip_copy(ip->dst_ip, dst_ip);

    /* Compute header checksum */
    ip->checksum = ip_checksum(ip, sizeof(ip_header_t));

    /* Append payload */
    memcpy(pkt + sizeof(ip_header_t), payload, payload_len);

    /* Determine next-hop: if not on same subnet, use gateway */
    uint8_t next_hop[4];
    int on_link = 1;
    for (int i = 0; i < 4; i++) {
        if ((dst_ip[i] & config.subnet[i]) != (config.ip[i] & config.subnet[i])) {
            on_link = 0;
            break;
        }
    }
    if (on_link)
        ip_copy(next_hop, dst_ip);
    else
        ip_copy(next_hop, config.gateway);

    /* ARP resolve next hop */
    arp_entry_t *entry = arp_lookup(next_hop);
    if (!entry) {
        /* Send ARP request and wait briefly */
        arp_send_request(next_hop);
        uint32_t start = timer_get_ticks();
        while (timer_get_ticks() - start < 1000) {  /* 1 second timeout */
            entry = arp_lookup(next_hop);
            if (entry) break;
            /* Busy-wait, interrupts will deliver ARP reply */
            asm volatile("hlt");
        }
        if (!entry) {
            return -1;  /* ARP resolution failed */
        }
    }

    return eth_send(entry->mac, ETH_TYPE_IP, pkt, total_len);
}

static void ip_handle(const uint8_t *pkt, uint16_t len) {
    if (len < sizeof(ip_header_t))
        return;

    const ip_header_t *ip = (const ip_header_t *)pkt;

    /* Basic validation */
    uint8_t version = (ip->version_ihl >> 4) & 0xF;
    uint8_t ihl = (ip->version_ihl & 0xF) * 4;  /* Header length in bytes */
    if (version != 4 || ihl < 20)
        return;

    uint16_t total_len = ntohs(ip->total_length);
    if (total_len > len)
        return;

    /* Check if it's addressed to us */
    if (!ip_eq(ip->dst_ip, config.ip))
        return;

    const uint8_t *payload = pkt + ihl;
    uint16_t payload_len = total_len - ihl;

    switch (ip->protocol) {
    case IP_PROTO_ICMP:
        /* Handle ICMP */
        if (payload_len < sizeof(icmp_header_t))
            return;

        const icmp_header_t *icmp = (const icmp_header_t *)payload;

        if (icmp->type == ICMP_ECHO_REQUEST) {
            /* Reply to ping: send back with swapped src/dst, type=0 */
            uint8_t reply[ETH_MTU];
            if (payload_len > ETH_MTU - sizeof(ip_header_t))
                return;

            memcpy(reply, payload, payload_len);
            icmp_header_t *rep = (icmp_header_t *)reply;
            rep->type = ICMP_ECHO_REPLY;
            rep->code = 0;
            rep->checksum = 0;
            rep->checksum = ip_checksum(reply, payload_len);

            ip_send(ip->src_ip, IP_PROTO_ICMP, reply, payload_len);
        } else if (icmp->type == ICMP_ECHO_REPLY) {
            /* Check if this matches our outstanding ping */
            if (ntohs(icmp->identifier) == ping_expected_id &&
                ntohs(icmp->sequence) == ping_expected_seq) {
                ping_reply_time = timer_get_ticks();
                ping_reply_received = 1;
            }
        }
        break;

    case IP_PROTO_UDP: {
        if (payload_len < sizeof(udp_header_t))
            return;

        const udp_header_t *udp = (const udp_header_t *)payload;
        uint16_t dst_port = ntohs(udp->dst_port);
        uint16_t src_port = ntohs(udp->src_port);
        uint16_t udp_len = ntohs(udp->length);

        if (udp_len < sizeof(udp_header_t) || udp_len > payload_len)
            return;

        const uint8_t *udp_data = payload + sizeof(udp_header_t);
        uint16_t udp_data_len = udp_len - sizeof(udp_header_t);

        /* Dispatch to registered handler */
        for (int i = 0; i < udp_handler_count; i++) {
            if (udp_handlers[i].port == dst_port && udp_handlers[i].handler) {
                udp_handlers[i].handler(ip->src_ip, src_port, udp_data, udp_data_len);
                break;
            }
        }
        break;
    }

    case IP_PROTO_TCP:
        tcp_handle_segment(ip->src_ip, payload, payload_len);
        break;

    default:
        /* Unsupported protocol — drop */
        break;
    }
}

/* --- Receive Handler (called from IRQ context) --- */

static void net_rx_handler(const void *data, uint16_t length) {
    if (length < ETH_HLEN)
        return;

    const eth_header_t *eth = (const eth_header_t *)data;
    uint16_t type = ntohs(eth->ethertype);
    const uint8_t *payload = (const uint8_t *)data + ETH_HLEN;
    uint16_t payload_len = length - ETH_HLEN;

    switch (type) {
    case ETH_TYPE_ARP:
        if (payload_len >= sizeof(arp_header_t))
            arp_handle((const arp_header_t *)payload);
        break;
    case ETH_TYPE_IP:
        ip_handle(payload, payload_len);
        break;
    default:
        break;
    }
}

/* --- Public API --- */

void net_init(void) {
    memset(&config, 0, sizeof(config));
    memset(arp_table, 0, sizeof(arp_table));

    if (!rtl8139_is_active()) {
        printf("[net] No NIC available\n");
        return;
    }

    /* Copy MAC from driver */
    memcpy(config.mac, rtl8139_get_mac(), ETH_ALEN);

    /* Default static config for QEMU user-mode networking */
    config.ip[0] = 10; config.ip[1] = 0; config.ip[2] = 2; config.ip[3] = 15;
    config.subnet[0] = 255; config.subnet[1] = 255; config.subnet[2] = 255; config.subnet[3] = 0;
    config.gateway[0] = 10; config.gateway[1] = 0; config.gateway[2] = 2; config.gateway[3] = 2;
    config.configured = 1;

    /* Register receive callback */
    rtl8139_set_rx_callback(net_rx_handler);

    printf("[net] Stack initialized: %d.%d.%d.%d\n",
           config.ip[0], config.ip[1], config.ip[2], config.ip[3]);
}

void net_configure(const uint8_t ip[4], const uint8_t subnet[4], const uint8_t gateway[4]) {
    ip_copy(config.ip, ip);
    ip_copy(config.subnet, subnet);
    ip_copy(config.gateway, gateway);
    config.configured = 1;
}

const net_config_t *net_get_config(void) {
    return &config;
}

int net_ping(const uint8_t dst_ip[4], uint16_t seq, uint32_t timeout_ms) {
    if (!config.configured)
        return -1;

    /* Build ICMP echo request */
    uint8_t pkt[sizeof(icmp_header_t) + 32];  /* Header + 32 bytes payload */
    icmp_header_t *icmp = (icmp_header_t *)pkt;

    ping_expected_id = 0x4D53;  /* "MS" for MOSS */
    ping_expected_seq = seq;
    ping_reply_received = 0;

    icmp->type       = ICMP_ECHO_REQUEST;
    icmp->code       = 0;
    icmp->checksum   = 0;
    icmp->identifier = htons(ping_expected_id);
    icmp->sequence   = htons(seq);

    /* Fill payload with pattern */
    for (int i = 0; i < 32; i++)
        pkt[sizeof(icmp_header_t) + i] = (uint8_t)(i + 'A');

    icmp->checksum = ip_checksum(pkt, sizeof(pkt));

    /* Send */
    uint32_t send_time = timer_get_ticks();
    int rc = ip_send(dst_ip, IP_PROTO_ICMP, pkt, sizeof(pkt));
    if (rc != 0)
        return -1;

    /* Wait for reply */
    while (timer_get_ticks() - send_time < timeout_ms) {
        if (ping_reply_received) {
            return (int)(ping_reply_time - send_time);  /* RTT in ms */
        }
        asm volatile("hlt");  /* Wait for next interrupt */
    }

    return -1;  /* Timeout */
}

/* --- Shell Command --- */

void ping_cmd(void) {
    if (argc < 2) {
        printf("Usage: ping <ip>\n");
        return;
    }

    if (!config.configured) {
        printf("Network not configured\n");
        return;
    }

    /* Parse IP address from argv[1] */
    uint8_t dst_ip[4] = {0};
    int octet = 0;
    int val = 0;
    const char *s = argv[1];

    while (*s) {
        if (*s == '.') {
            if (octet >= 3) { printf("Invalid IP\n"); return; }
            dst_ip[octet++] = (uint8_t)val;
            val = 0;
        } else if (*s >= '0' && *s <= '9') {
            val = val * 10 + (*s - '0');
            if (val > 255) { printf("Invalid IP\n"); return; }
        } else {
            printf("Invalid IP\n");
            return;
        }
        s++;
    }
    if (octet != 3) { printf("Invalid IP\n"); return; }
    dst_ip[3] = (uint8_t)val;

    printf("PING %d.%d.%d.%d: 32 data bytes\n",
           dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);

    int sent = 0, received = 0;
    for (uint16_t seq = 1; seq <= 4; seq++) {
        sent++;
        int rtt = net_ping(dst_ip, seq, 3000);  /* 3 second timeout */
        if (rtt >= 0) {
            received++;
            printf("Reply from %d.%d.%d.%d: seq=%d time=%dms\n",
                   dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3], seq, rtt);
        } else {
            printf("Request timeout for seq %d\n", seq);
        }
        /* Brief delay between pings */
        if (seq < 4) {
            uint32_t wait_start = timer_get_ticks();
            while (timer_get_ticks() - wait_start < 1000)
                asm volatile("hlt");
        }
    }

    printf("--- %d.%d.%d.%d ping statistics ---\n", dst_ip[0], dst_ip[1], dst_ip[2], dst_ip[3]);
    printf("%d packets transmitted, %d received, %d%% packet loss\n",
           sent, received, ((sent - received) * 100) / sent);
}

/* --- UDP --- */

int udp_send(const uint8_t dst_ip[4], uint16_t src_port, uint16_t dst_port,
             const void *data, uint16_t data_len) {
    if (!config.configured)
        return -1;

    uint16_t udp_total = sizeof(udp_header_t) + data_len;
    if (udp_total > ETH_MTU - sizeof(ip_header_t))
        return -1;

    uint8_t buf[ETH_MTU];
    udp_header_t *udp = (udp_header_t *)buf;
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length   = htons(udp_total);
    udp->checksum = 0;  /* Optional in IPv4 */

    memcpy(buf + sizeof(udp_header_t), data, data_len);

    return ip_send(dst_ip, IP_PROTO_UDP, buf, udp_total);
}

void udp_register_handler(uint16_t port, udp_rx_handler_t handler) {
    if (udp_handler_count >= UDP_MAX_HANDLERS)
        return;

    /* Check if already registered */
    for (int i = 0; i < udp_handler_count; i++) {
        if (udp_handlers[i].port == port) {
            udp_handlers[i].handler = handler;
            return;
        }
    }

    udp_handlers[udp_handler_count].port = port;
    udp_handlers[udp_handler_count].handler = handler;
    udp_handler_count++;
}

/* --- DNS Resolver --- */

/* DNS response handler (called from IRQ context via UDP dispatch) */
static void dns_rx_handler(const uint8_t src_ip[4], uint16_t src_port,
                           const void *data, uint16_t len) {
    (void)src_ip;
    (void)src_port;

    if (len < sizeof(dns_header_t))
        return;

    const dns_header_t *hdr = (const dns_header_t *)data;

    /* Check transaction ID matches */
    if (ntohs(hdr->id) != dns_transaction_id)
        return;

    /* Check it's a response */
    if (!(ntohs(hdr->flags) & DNS_FLAG_QR))
        return;

    uint16_t ancount = ntohs(hdr->ancount);
    if (ancount == 0)
        return;

    /* Skip the question section: walk past the query name + QTYPE + QCLASS */
    const uint8_t *ptr = (const uint8_t *)data + sizeof(dns_header_t);
    const uint8_t *end = (const uint8_t *)data + len;

    /* Skip QNAME (sequence of labels terminated by 0) */
    while (ptr < end && *ptr != 0) {
        if ((*ptr & 0xC0) == 0xC0) {
            ptr += 2;  /* Pointer: 2 bytes */
            goto skip_done;
        }
        ptr += 1 + *ptr;  /* Length byte + label */
    }
    ptr++;  /* Skip the null terminator */
skip_done:
    ptr += 4;  /* Skip QTYPE (2) + QCLASS (2) */

    /* Parse answer records looking for type A */
    for (uint16_t i = 0; i < ancount && ptr + 12 <= end; i++) {
        /* Skip NAME (may be a pointer) */
        if ((*ptr & 0xC0) == 0xC0) {
            ptr += 2;
        } else {
            while (ptr < end && *ptr != 0)
                ptr += 1 + *ptr;
            ptr++;
        }

        if (ptr + 10 > end)
            return;

        uint16_t rtype = (ptr[0] << 8) | ptr[1];
        /* uint16_t rclass = (ptr[2] << 8) | ptr[3]; */
        /* uint32_t ttl = ... */
        uint16_t rdlength = (ptr[8] << 8) | ptr[9];
        ptr += 10;

        if (ptr + rdlength > end)
            return;

        if (rtype == DNS_TYPE_A && rdlength == 4) {
            /* Found an A record! */
            memcpy(dns_resolved_ip, ptr, 4);
            dns_reply_received = 1;
            return;
        }

        ptr += rdlength;
    }
}

/* Encode a hostname into DNS wire format (e.g., "www.google.com" → \3www\6google\3com\0) */
static int dns_encode_name(const char *hostname, uint8_t *out, int max_len) {
    int pos = 0;
    const char *s = hostname;

    while (*s) {
        /* Find next dot or end */
        const char *dot = s;
        while (*dot && *dot != '.')
            dot++;

        int label_len = (int)(dot - s);
        if (label_len == 0 || label_len > 63)
            return -1;
        if (pos + 1 + label_len >= max_len)
            return -1;

        out[pos++] = (uint8_t)label_len;
        memcpy(out + pos, s, label_len);
        pos += label_len;

        s = dot;
        if (*s == '.')
            s++;
    }

    if (pos + 1 >= max_len)
        return -1;
    out[pos++] = 0;  /* Terminator */
    return pos;
}

int dns_resolve(const char *hostname, uint8_t out_ip[4]) {
    if (!config.configured)
        return -1;

    /* QEMU's DNS server is at 10.0.2.3 */
    uint8_t dns_server[4] = {10, 0, 2, 3};

    /* Register our DNS response handler on a high ephemeral port */
    uint16_t local_port = 49152 + (ip_id_counter & 0xFF);
    udp_register_handler(local_port, dns_rx_handler);

    /* Build DNS query packet */
    uint8_t pkt[512];
    dns_header_t *hdr = (dns_header_t *)pkt;

    dns_transaction_id = ip_id_counter++;
    dns_reply_received = 0;

    hdr->id      = htons(dns_transaction_id);
    hdr->flags   = htons(DNS_FLAG_RD);  /* Recursion desired */
    hdr->qdcount = htons(1);
    hdr->ancount = 0;
    hdr->nscount = 0;
    hdr->arcount = 0;

    /* Encode the question */
    int name_len = dns_encode_name(hostname, pkt + sizeof(dns_header_t),
                                   (int)(sizeof(pkt) - sizeof(dns_header_t) - 4));
    if (name_len < 0)
        return -1;

    uint8_t *qtype_ptr = pkt + sizeof(dns_header_t) + name_len;
    qtype_ptr[0] = 0; qtype_ptr[1] = DNS_TYPE_A;    /* QTYPE = A */
    qtype_ptr[2] = 0; qtype_ptr[3] = DNS_CLASS_IN;  /* QCLASS = IN */

    uint16_t query_len = (uint16_t)(sizeof(dns_header_t) + name_len + 4);

    /* Send the query */
    int rc = udp_send(dns_server, local_port, DNS_PORT, pkt, query_len);
    if (rc != 0)
        return -1;

    /* Wait for reply (5 second timeout) */
    uint32_t start = timer_get_ticks();
    while (timer_get_ticks() - start < 5000) {
        if (dns_reply_received) {
            memcpy(out_ip, dns_resolved_ip, 4);
            return 0;
        }
        asm volatile("hlt");
    }

    return -1;  /* Timeout */
}

/* --- Shell Commands --- */

void resolve_cmd(void) {
    if (argc < 2) {
        printf("Usage: resolve <hostname>\n");
        return;
    }

    if (!config.configured) {
        printf("Network not configured\n");
        return;
    }

    printf("Resolving %s...\n", argv[1]);

    uint8_t ip[4];
    int rc = dns_resolve(argv[1], ip);
    if (rc == 0) {
        printf("%s -> %d.%d.%d.%d\n", argv[1], ip[0], ip[1], ip[2], ip[3]);
    } else {
        printf("DNS resolution failed\n");
    }
}
