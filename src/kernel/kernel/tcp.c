/*
 * MOSS TCP Implementation
 * Minimal client-only TCP supporting active open, data transfer, and close.
 * No listen/accept, no out-of-order reassembly, no congestion control.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/net.h>
#include <kernel/kernel.h>

/* From net.c */
extern int ip_send(const uint8_t dst_ip[4], uint8_t protocol,
                   const void *payload, uint16_t payload_len);
extern uint16_t ip_checksum(const void *data, uint16_t length);
extern uint32_t timer_get_ticks(void);

/* Shell args */
extern int   argc;
extern char *argv[];

/* From net.c */
extern net_config_t config;

/* --- TCP Connection State --- */

typedef struct {
    int      state;
    uint8_t  remote_ip[4];
    uint16_t local_port;
    uint16_t remote_port;

    /* Sequence numbers */
    uint32_t snd_nxt;      /* Next sequence number to send */
    uint32_t snd_una;      /* Oldest unacknowledged byte */
    uint32_t rcv_nxt;      /* Next expected byte from remote */

    /* Remote window */
    uint16_t snd_wnd;

    /* Receive ring buffer */
    uint8_t  rx_buf[TCP_RX_BUF_SIZE];
    uint16_t rx_head;      /* Write position (producer: IRQ) */
    uint16_t rx_tail;      /* Read position (consumer: tcp_recv) */

    /* Retransmit buffer (last sent segment, for simple retransmit) */
    uint8_t  retx_buf[TCP_TX_BUF_SIZE];
    uint16_t retx_len;
    uint32_t retx_time;    /* Tick when last sent */

    /* Flags */
    volatile int syn_acked;
    volatile int fin_received;
    volatile int rst_received;
} tcp_connection_t;

static tcp_connection_t connections[TCP_MAX_CONNECTIONS];
static uint16_t next_ephemeral_port = 49152;

/* --- Internal Helpers --- */

static uint16_t tcp_alloc_port(void) {
    return next_ephemeral_port++;
}

static int tcp_find_connection(const uint8_t remote_ip[4], uint16_t remote_port,
                               uint16_t local_port) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (connections[i].state != TCP_STATE_CLOSED &&
            connections[i].local_port == local_port &&
            connections[i].remote_port == remote_port &&
            memcmp(connections[i].remote_ip, remote_ip, 4) == 0) {
            return i;
        }
    }
    return -1;
}

static int tcp_alloc_connection(void) {
    for (int i = 0; i < TCP_MAX_CONNECTIONS; i++) {
        if (connections[i].state == TCP_STATE_CLOSED)
            return i;
    }
    return -1;
}

static uint16_t rx_buf_available(tcp_connection_t *c) {
    return (uint16_t)((c->rx_head - c->rx_tail) % TCP_RX_BUF_SIZE);
}

/* TCP pseudo-header for checksum calculation */
typedef struct {
    uint8_t  src_ip[4];
    uint8_t  dst_ip[4];
    uint8_t  zero;
    uint8_t  protocol;
    uint16_t tcp_length;
} __attribute__((packed)) tcp_pseudo_header_t;

static uint16_t tcp_checksum(const uint8_t src_ip[4], const uint8_t dst_ip[4],
                             const void *tcp_segment, uint16_t tcp_len) {
    /* Build pseudo-header + TCP segment for checksumming */
    uint8_t buf[sizeof(tcp_pseudo_header_t) + 1500];
    if (tcp_len > 1500)
        return 0;

    tcp_pseudo_header_t *ph = (tcp_pseudo_header_t *)buf;
    memcpy(ph->src_ip, src_ip, 4);
    memcpy(ph->dst_ip, dst_ip, 4);
    ph->zero = 0;
    ph->protocol = IP_PROTO_TCP;
    ph->tcp_length = htons(tcp_len);

    memcpy(buf + sizeof(tcp_pseudo_header_t), tcp_segment, tcp_len);

    return ip_checksum(buf, (uint16_t)(sizeof(tcp_pseudo_header_t) + tcp_len));
}

/* Send a TCP segment with given flags and optional data */
static int tcp_send_segment(tcp_connection_t *c, uint8_t flags,
                            const void *data, uint16_t data_len) {
    uint8_t seg[sizeof(tcp_header_t) + TCP_TX_BUF_SIZE];
    uint16_t tcp_len = sizeof(tcp_header_t) + data_len;

    if (data_len > TCP_TX_BUF_SIZE)
        return -1;

    tcp_header_t *tcp = (tcp_header_t *)seg;
    tcp->src_port    = htons(c->local_port);
    tcp->dst_port    = htons(c->remote_port);
    tcp->seq_num     = htonl(c->snd_nxt);
    tcp->ack_num     = htonl(c->rcv_nxt);
    tcp->data_offset = (sizeof(tcp_header_t) / 4) << 4;  /* 5 dwords, no options */
    tcp->flags       = flags;
    tcp->window      = htons(TCP_RX_BUF_SIZE - rx_buf_available(c));
    tcp->checksum    = 0;
    tcp->urgent_ptr  = 0;

    if (data_len > 0)
        memcpy(seg + sizeof(tcp_header_t), data, data_len);

    /* Compute checksum over pseudo-header + segment */
    tcp->checksum = tcp_checksum(config.ip, c->remote_ip, seg, tcp_len);

    /* Save for potential retransmission */
    if (data_len > 0 || (flags & (TCP_SYN | TCP_FIN))) {
        memcpy(c->retx_buf, seg, tcp_len);
        c->retx_len = tcp_len;
        c->retx_time = timer_get_ticks();
    }

    return ip_send(c->remote_ip, IP_PROTO_TCP, seg, tcp_len);
}

/* Send a RST to reject a connection */
static void tcp_send_rst(const uint8_t dst_ip[4], uint16_t src_port,
                         uint16_t dst_port, uint32_t seq, uint32_t ack) {
    uint8_t seg[sizeof(tcp_header_t)];
    tcp_header_t *tcp = (tcp_header_t *)seg;

    tcp->src_port    = htons(src_port);
    tcp->dst_port    = htons(dst_port);
    tcp->seq_num     = htonl(seq);
    tcp->ack_num     = htonl(ack);
    tcp->data_offset = (sizeof(tcp_header_t) / 4) << 4;
    tcp->flags       = TCP_RST | TCP_ACK;
    tcp->window      = 0;
    tcp->checksum    = 0;
    tcp->urgent_ptr  = 0;

    tcp->checksum = tcp_checksum(config.ip, dst_ip, seg, sizeof(tcp_header_t));
    ip_send(dst_ip, IP_PROTO_TCP, seg, sizeof(tcp_header_t));
}

/* --- TCP Segment Handler (called from ip_handle via net.c) --- */

void tcp_handle_segment(const uint8_t *src_ip, const uint8_t *payload, uint16_t len) {
    if (len < sizeof(tcp_header_t))
        return;

    const tcp_header_t *tcp = (const tcp_header_t *)payload;
    uint16_t src_port = ntohs(tcp->src_port);
    uint16_t dst_port = ntohs(tcp->dst_port);
    uint32_t seq = ntohl(tcp->seq_num);
    uint32_t ack = ntohl(tcp->ack_num);
    uint8_t  flags = tcp->flags;
    uint8_t  hdr_len = (tcp->data_offset >> 4) * 4;

    if (hdr_len < sizeof(tcp_header_t) || hdr_len > len)
        return;

    const uint8_t *data = payload + hdr_len;
    uint16_t data_len = len - hdr_len;

    /* Find matching connection */
    int idx = tcp_find_connection(src_ip, src_port, dst_port);
    if (idx < 0) {
        /* No connection found — send RST */
        if (!(flags & TCP_RST)) {
            tcp_send_rst(src_ip, dst_port, src_port, ack, seq + data_len + 1);
        }
        return;
    }

    tcp_connection_t *c = &connections[idx];

    /* Handle RST */
    if (flags & TCP_RST) {
        c->rst_received = 1;
        c->state = TCP_STATE_CLOSED;
        return;
    }

    switch (c->state) {
    case TCP_STATE_SYN_SENT:
        /* Expecting SYN-ACK */
        if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
            if (ack == c->snd_nxt) {
                c->rcv_nxt = seq + 1;
                c->snd_una = ack;
                c->snd_wnd = ntohs(tcp->window);
                c->state = TCP_STATE_ESTABLISHED;
                c->syn_acked = 1;

                /* Send ACK */
                tcp_send_segment(c, TCP_ACK, NULL, 0);
            }
        }
        break;

    case TCP_STATE_ESTABLISHED:
        /* Process ACK */
        if (flags & TCP_ACK) {
            if (ack > c->snd_una && ack <= c->snd_nxt) {
                c->snd_una = ack;
                c->retx_len = 0;  /* ACKed, clear retransmit buffer */
            }
            c->snd_wnd = ntohs(tcp->window);
        }

        /* Process incoming data */
        if (data_len > 0) {
            if (seq == c->rcv_nxt) {
                /* In-order data — copy to receive buffer */
                for (uint16_t i = 0; i < data_len; i++) {
                    uint16_t next_head = (c->rx_head + 1) % TCP_RX_BUF_SIZE;
                    if (next_head == c->rx_tail)
                        break;  /* Buffer full, drop remaining */
                    c->rx_buf[c->rx_head] = data[i];
                    c->rx_head = next_head;
                }
                c->rcv_nxt = seq + data_len;

                /* Send ACK for received data */
                tcp_send_segment(c, TCP_ACK, NULL, 0);
            } else if (seq > c->rcv_nxt) {
                /* Out of order — just ACK what we have (no reassembly) */
                tcp_send_segment(c, TCP_ACK, NULL, 0);
            }
        }

        /* Handle FIN */
        if (flags & TCP_FIN) {
            c->rcv_nxt = seq + data_len + 1;
            c->fin_received = 1;
            c->state = TCP_STATE_CLOSE_WAIT;

            /* ACK the FIN */
            tcp_send_segment(c, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_STATE_FIN_WAIT_1:
        if (flags & TCP_ACK) {
            if (ack == c->snd_nxt) {
                c->snd_una = ack;
                if (flags & TCP_FIN) {
                    c->rcv_nxt = seq + data_len + 1;
                    c->state = TCP_STATE_TIME_WAIT;
                    tcp_send_segment(c, TCP_ACK, NULL, 0);
                } else {
                    c->state = TCP_STATE_FIN_WAIT_2;
                }
            }
        }
        /* Also handle data arriving during FIN_WAIT_1 */
        if (data_len > 0 && seq == c->rcv_nxt) {
            for (uint16_t i = 0; i < data_len; i++) {
                uint16_t next_head = (c->rx_head + 1) % TCP_RX_BUF_SIZE;
                if (next_head == c->rx_tail) break;
                c->rx_buf[c->rx_head] = data[i];
                c->rx_head = next_head;
            }
            c->rcv_nxt = seq + data_len;
        }
        break;

    case TCP_STATE_FIN_WAIT_2:
        /* Waiting for FIN from remote */
        if (data_len > 0 && seq == c->rcv_nxt) {
            for (uint16_t i = 0; i < data_len; i++) {
                uint16_t next_head = (c->rx_head + 1) % TCP_RX_BUF_SIZE;
                if (next_head == c->rx_tail) break;
                c->rx_buf[c->rx_head] = data[i];
                c->rx_head = next_head;
            }
            c->rcv_nxt = seq + data_len;
            tcp_send_segment(c, TCP_ACK, NULL, 0);
        }
        if (flags & TCP_FIN) {
            c->rcv_nxt = seq + data_len + 1;
            c->fin_received = 1;
            c->state = TCP_STATE_TIME_WAIT;
            tcp_send_segment(c, TCP_ACK, NULL, 0);
        }
        break;

    case TCP_STATE_LAST_ACK:
        if ((flags & TCP_ACK) && ack == c->snd_nxt) {
            c->state = TCP_STATE_CLOSED;
        }
        break;

    case TCP_STATE_CLOSE_WAIT:
        /* Waiting for application to close */
        break;

    case TCP_STATE_TIME_WAIT:
        /* Wait briefly then close (skip 2MSL for simplicity) */
        c->state = TCP_STATE_CLOSED;
        break;

    default:
        break;
    }
}

/* --- Public TCP API --- */

tcp_conn_t tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port, uint32_t timeout_ms) {
    int idx = tcp_alloc_connection();
    if (idx < 0)
        return -1;

    tcp_connection_t *c = &connections[idx];
    memset(c, 0, sizeof(tcp_connection_t));

    memcpy(c->remote_ip, dst_ip, 4);
    c->local_port  = tcp_alloc_port();
    c->remote_port = dst_port;
    c->state       = TCP_STATE_SYN_SENT;
    c->syn_acked   = 0;

    /* Use ticks as initial sequence number (simple, not cryptographically random) */
    c->snd_nxt = timer_get_ticks() * 64;
    c->snd_una = c->snd_nxt;

    /* Send SYN */
    c->snd_nxt++;  /* SYN consumes one sequence number */
    tcp_header_t syn_seg;
    memset(&syn_seg, 0, sizeof(syn_seg));
    syn_seg.src_port    = htons(c->local_port);
    syn_seg.dst_port    = htons(c->remote_port);
    syn_seg.seq_num     = htonl(c->snd_nxt - 1);
    syn_seg.ack_num     = 0;
    syn_seg.data_offset = (sizeof(tcp_header_t) / 4) << 4;
    syn_seg.flags       = TCP_SYN;
    syn_seg.window      = htons(TCP_RX_BUF_SIZE);
    syn_seg.checksum    = 0;
    syn_seg.urgent_ptr  = 0;

    syn_seg.checksum = tcp_checksum(config.ip, c->remote_ip,
                                    &syn_seg, sizeof(tcp_header_t));

    /* Save for retransmit */
    memcpy(c->retx_buf, &syn_seg, sizeof(tcp_header_t));
    c->retx_len = sizeof(tcp_header_t);
    c->retx_time = timer_get_ticks();

    ip_send(dst_ip, IP_PROTO_TCP, &syn_seg, sizeof(tcp_header_t));

    /* Wait for SYN-ACK */
    uint32_t start = timer_get_ticks();
    uint32_t last_retx = start;
    while (timer_get_ticks() - start < timeout_ms) {
        if (c->syn_acked)
            return idx;
        if (c->rst_received) {
            c->state = TCP_STATE_CLOSED;
            return -1;
        }
        /* Retransmit SYN every 1000ms */
        if (timer_get_ticks() - last_retx > 1000) {
            ip_send(dst_ip, IP_PROTO_TCP, c->retx_buf, c->retx_len);
            last_retx = timer_get_ticks();
        }
        asm volatile("hlt");
    }

    /* Timeout */
    c->state = TCP_STATE_CLOSED;
    return -1;
}

int tcp_send(tcp_conn_t conn, const void *data, uint16_t len) {
    if (conn < 0 || conn >= TCP_MAX_CONNECTIONS)
        return -1;

    tcp_connection_t *c = &connections[conn];
    if (c->state != TCP_STATE_ESTABLISHED)
        return -1;

    /* Send in MSS-sized chunks (use 1460 = ETH_MTU - IP_HDR - TCP_HDR) */
    const uint16_t mss = 1460;
    const uint8_t *ptr = (const uint8_t *)data;
    uint16_t remaining = len;
    uint16_t total_sent = 0;

    while (remaining > 0) {
        uint16_t chunk = remaining > mss ? mss : remaining;

        /* Wait for window space */
        uint32_t wait_start = timer_get_ticks();
        while (c->snd_nxt - c->snd_una >= c->snd_wnd && c->snd_wnd > 0) {
            if (timer_get_ticks() - wait_start > 5000)
                return total_sent > 0 ? total_sent : -1;
            if (c->rst_received || c->state != TCP_STATE_ESTABLISHED)
                return -1;
            asm volatile("hlt");
        }

        tcp_send_segment(c, TCP_ACK | TCP_PSH, ptr, chunk);
        c->snd_nxt += chunk;
        ptr += chunk;
        remaining -= chunk;
        total_sent += chunk;

        /* Wait for ACK with simple retransmit */
        uint32_t ack_start = timer_get_ticks();
        while (c->snd_una < c->snd_nxt) {
            if (timer_get_ticks() - ack_start > 5000) {
                /* Retransmit */
                if (c->retx_len > 0) {
                    ip_send(c->remote_ip, IP_PROTO_TCP, c->retx_buf, c->retx_len);
                    ack_start = timer_get_ticks();
                }
            }
            if (c->snd_una >= c->snd_nxt)
                break;
            if (c->rst_received || c->state != TCP_STATE_ESTABLISHED)
                return total_sent > 0 ? total_sent : -1;
            asm volatile("hlt");
        }
    }

    return total_sent;
}

int tcp_recv(tcp_conn_t conn, void *buf, uint16_t max_len, uint32_t timeout_ms) {
    if (conn < 0 || conn >= TCP_MAX_CONNECTIONS)
        return -1;

    tcp_connection_t *c = &connections[conn];

    /* Wait for data */
    uint32_t start = timer_get_ticks();
    while (c->rx_head == c->rx_tail) {
        /* Check if connection is still alive */
        if (c->fin_received || c->rst_received || c->state == TCP_STATE_CLOSED)
            return 0;  /* EOF */
        if (timeout_ms > 0 && timer_get_ticks() - start >= timeout_ms)
            return 0;  /* Timeout, no data */
        asm volatile("hlt");
    }

    /* Copy available data from ring buffer */
    uint16_t copied = 0;
    uint8_t *dst = (uint8_t *)buf;
    while (copied < max_len && c->rx_tail != c->rx_head) {
        dst[copied++] = c->rx_buf[c->rx_tail];
        c->rx_tail = (c->rx_tail + 1) % TCP_RX_BUF_SIZE;
    }

    return copied;
}

void tcp_close(tcp_conn_t conn) {
    if (conn < 0 || conn >= TCP_MAX_CONNECTIONS)
        return;

    tcp_connection_t *c = &connections[conn];

    if (c->state == TCP_STATE_ESTABLISHED) {
        /* Send FIN */
        tcp_send_segment(c, TCP_FIN | TCP_ACK, NULL, 0);
        c->snd_nxt++;
        c->state = TCP_STATE_FIN_WAIT_1;

        /* Wait for ACK of our FIN (up to 5 seconds) */
        uint32_t start = timer_get_ticks();
        while (c->state != TCP_STATE_CLOSED &&
               c->state != TCP_STATE_TIME_WAIT &&
               timer_get_ticks() - start < 5000) {
            if (c->rst_received) break;
            asm volatile("hlt");
        }
    } else if (c->state == TCP_STATE_CLOSE_WAIT) {
        /* Remote already sent FIN, we send ours */
        tcp_send_segment(c, TCP_FIN | TCP_ACK, NULL, 0);
        c->snd_nxt++;
        c->state = TCP_STATE_LAST_ACK;

        uint32_t start = timer_get_ticks();
        while (c->state == TCP_STATE_LAST_ACK &&
               timer_get_ticks() - start < 5000) {
            asm volatile("hlt");
        }
    }

    c->state = TCP_STATE_CLOSED;
}

int tcp_is_connected(tcp_conn_t conn) {
    if (conn < 0 || conn >= TCP_MAX_CONNECTIONS)
        return 0;
    return connections[conn].state == TCP_STATE_ESTABLISHED;
}

/* --- HTTP GET (Simple) --- */

void wget_cmd(void) {
    if (argc < 2) {
        printf("Usage: wget <url>\n");
        printf("  e.g.: wget http://example.com/\n");
        return;
    }

    /* Parse URL: http://host/path */
    const char *url = argv[1];
    if (memcmp(url, "http://", 7) != 0) {
        printf("Only http:// supported\n");
        return;
    }
    url += 7;  /* Skip "http://" */

    /* Extract host and path */
    char host[128];
    char path[256];
    int hi = 0;
    const char *p = url;

    while (*p && *p != '/' && hi < 127)
        host[hi++] = *p++;
    host[hi] = '\0';

    if (*p == '/') {
        int pi = 0;
        while (*p && pi < 255)
            path[pi++] = *p++;
        path[pi] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }

    /* Parse optional port from host (host:port) */
    uint16_t port = 80;
    char *colon = NULL;
    for (int i = 0; host[i]; i++) {
        if (host[i] == ':') { colon = &host[i]; break; }
    }
    if (colon) {
        *colon = '\0';
        port = 0;
        for (const char *pp = colon + 1; *pp >= '0' && *pp <= '9'; pp++)
            port = port * 10 + (*pp - '0');
        if (port == 0) port = 80;
    }

    printf("Connecting to %s:%d%s\n", host, port, path);

    /* Resolve hostname */
    uint8_t server_ip[4];

    /* Check if host is already an IP address */
    int is_ip = 1;
    for (int i = 0; host[i]; i++) {
        if (host[i] != '.' && (host[i] < '0' || host[i] > '9')) {
            is_ip = 0;
            break;
        }
    }

    if (is_ip) {
        /* Parse IP directly */
        int octet = 0, val = 0;
        for (const char *s = host; ; s++) {
            if (*s == '.' || *s == '\0') {
                server_ip[octet++] = (uint8_t)val;
                val = 0;
                if (*s == '\0') break;
            } else {
                val = val * 10 + (*s - '0');
            }
        }
    } else {
        printf("Resolving %s...\n", host);
        if (dns_resolve(host, server_ip) != 0) {
            printf("DNS resolution failed\n");
            return;
        }
        printf("Resolved to %d.%d.%d.%d\n",
               server_ip[0], server_ip[1], server_ip[2], server_ip[3]);
    }

    /* TCP connect */
    tcp_conn_t conn = tcp_connect(server_ip, port, 10000);
    if (conn < 0) {
        printf("Connection failed\n");
        return;
    }
    printf("Connected!\n");

    /* Build HTTP GET request */
    char request[512];
    int req_len = 0;
    memcpy(request, "GET ", 4); req_len += 4;
    int plen = strlen(path);
    memcpy(request + req_len, path, plen); req_len += plen;
    memcpy(request + req_len, " HTTP/1.0\r\nHost: ", 17); req_len += 17;
    int hlen = strlen(host);
    memcpy(request + req_len, host, hlen); req_len += hlen;
    memcpy(request + req_len, "\r\nConnection: close\r\n\r\n", 23); req_len += 23;

    /* Send request */
    int sent = tcp_send(conn, request, (uint16_t)req_len);
    if (sent < 0) {
        printf("Send failed\n");
        tcp_close(conn);
        return;
    }

    /* Receive and print response */
    char buf[512];
    int total = 0;
    while (1) {
        int n = tcp_recv(conn, buf, sizeof(buf) - 1, 10000);
        if (n <= 0)
            break;
        buf[n] = '\0';
        printf("%s", buf);
        total += n;
    }

    printf("\n--- %d bytes received ---\n", total);
    tcp_close(conn);
}
