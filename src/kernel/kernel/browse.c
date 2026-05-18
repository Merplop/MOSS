/*
 * MOSS Web Browser
 * Minimal text-mode web browser. Fetches HTTP pages, strips HTML,
 * renders text with word wrapping, and supports link navigation.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <kernel/net.h>
#include <kernel/kernel.h>
#include <kernel/tty.h>
#include <kernel/keyboard.h>

/* TCP API */
extern tcp_conn_t tcp_connect(const uint8_t dst_ip[4], uint16_t dst_port, uint32_t timeout_ms);
extern int tcp_send(tcp_conn_t conn, const void *data, uint16_t len);
extern int tcp_recv(tcp_conn_t conn, void *buf, uint16_t max_len, uint32_t timeout_ms);
extern void tcp_close(tcp_conn_t conn);

/* DNS */
extern int dns_resolve(const char *hostname, uint8_t out_ip[4]);

/* Keyboard */
extern uint8_t get_key(void);

/* Shell args */
extern int   argc;
extern char *argv[];

/* Timer */
extern uint32_t timer_get_ticks(void);

/* --- Configuration --- */

#define MAX_PAGE_SIZE   32768   /* Max HTML we'll store (32K) */
#define MAX_RENDER_LINES 1024  /* Max rendered text lines */
#define MAX_LINE_LEN    256    /* Max chars per rendered line */
#define MAX_LINKS       64     /* Max links tracked per page */
#define MAX_URL_LEN     256
#define MAX_HISTORY     16

/* --- Data Structures --- */

typedef struct {
    char url[MAX_URL_LEN];         /* Full URL of the link target */
    int  line;                     /* Line number where link appears */
} link_entry_t;

/* --- Static Buffers --- */

static char page_buf[MAX_PAGE_SIZE];   /* Raw HTML content */
static int  page_len = 0;

/* Rendered lines — stored as offsets into a text buffer */
static char render_buf[MAX_PAGE_SIZE]; /* Rendered plain text */
static int  render_len = 0;
static int  line_offsets[MAX_RENDER_LINES];  /* Offset of each line start in render_buf */
static int  line_count = 0;

/* Links */
static link_entry_t links[MAX_LINKS];
static int link_count = 0;

/* Navigation */
static char history[MAX_HISTORY][MAX_URL_LEN];
static int  history_pos = 0;
static int  history_count = 0;

static char current_url[MAX_URL_LEN];
static char current_host[128];
static char current_path[256];
static uint16_t current_port = 80;

/* --- URL Parsing --- */

static int parse_url(const char *url, char *host, int host_max,
                     char *path, int path_max, uint16_t *port) {
    const char *p = url;
    *port = 80;

    /* Skip http:// */
    if (p[0] == 'h' && p[1] == 't' && p[2] == 't' && p[3] == 'p' &&
        p[4] == ':' && p[5] == '/' && p[6] == '/')
        p += 7;

    /* Extract host */
    int hi = 0;
    while (*p && *p != '/' && *p != ':' && hi < host_max - 1)
        host[hi++] = *p++;
    host[hi] = '\0';

    /* Optional port */
    if (*p == ':') {
        p++;
        *port = 0;
        while (*p >= '0' && *p <= '9')
            *port = *port * 10 + (*p++ - '0');
        if (*port == 0) *port = 80;
    }

    /* Path */
    if (*p == '/') {
        int pi = 0;
        while (*p && pi < path_max - 1)
            path[pi++] = *p++;
        path[pi] = '\0';
    } else {
        path[0] = '/'; path[1] = '\0';
    }

    return 0;
}

/* Resolve a relative URL against the current page's base */
static void resolve_relative_url(const char *href, char *out, int out_max) {
    if (href[0] == 'h' && href[1] == 't' && href[2] == 't' && href[3] == 'p') {
        /* Absolute URL */
        int i = 0;
        while (href[i] && i < out_max - 1)
            out[i] = href[i], i++;
        out[i] = '\0';
    } else if (href[0] == '/') {
        /* Absolute path */
        int pos = 0;
        memcpy(out, "http://", 7); pos += 7;
        int hlen = strlen(current_host);
        memcpy(out + pos, current_host, hlen); pos += hlen;
        if (current_port != 80) {
            out[pos++] = ':';
            /* Simple port to string */
            uint16_t p = current_port;
            char pbuf[6]; int pi = 0;
            do { pbuf[pi++] = '0' + (p % 10); p /= 10; } while (p > 0);
            for (int j = pi - 1; j >= 0; j--)
                out[pos++] = pbuf[j];
        }
        int hl = strlen(href);
        if (pos + hl < out_max) {
            memcpy(out + pos, href, hl);
            pos += hl;
        }
        out[pos] = '\0';
    } else {
        /* Relative path — append to current directory */
        int pos = 0;
        memcpy(out, "http://", 7); pos += 7;
        int hlen = strlen(current_host);
        memcpy(out + pos, current_host, hlen); pos += hlen;

        /* Find last '/' in current_path */
        int last_slash = 0;
        for (int i = 0; current_path[i]; i++)
            if (current_path[i] == '/') last_slash = i;

        memcpy(out + pos, current_path, last_slash + 1); pos += last_slash + 1;
        int rl = strlen(href);
        if (pos + rl < out_max) {
            memcpy(out + pos, href, rl); pos += rl;
        }
        out[pos] = '\0';
    }
}

/* --- HTTP Fetch --- */

static int fetch_page(const char *url) {
    char host[128], path[256];
    uint16_t port;

    parse_url(url, host, sizeof(host), path, sizeof(path), &port);

    /* Save current context */
    int hlen = strlen(host);
    memcpy(current_host, host, hlen + 1);
    int plen = strlen(path);
    memcpy(current_path, path, plen + 1);
    current_port = port;
    int ulen = strlen(url);
    if (ulen >= MAX_URL_LEN) ulen = MAX_URL_LEN - 1;
    memcpy(current_url, url, ulen);
    current_url[ulen] = '\0';

    /* Resolve IP */
    uint8_t server_ip[4];
    int is_ip = 1;
    for (int i = 0; host[i]; i++) {
        if (host[i] != '.' && (host[i] < '0' || host[i] > '9')) {
            is_ip = 0; break;
        }
    }

    if (is_ip) {
        int octet = 0, val = 0;
        for (const char *s = host; ; s++) {
            if (*s == '.' || *s == '\0') {
                server_ip[octet++] = (uint8_t)val; val = 0;
                if (!*s) break;
            } else {
                val = val * 10 + (*s - '0');
            }
        }
    } else {
        if (dns_resolve(host, server_ip) != 0) {
            printf("DNS failed for %s\n", host);
            return -1;
        }
    }

    /* Connect */
    tcp_conn_t conn = tcp_connect(server_ip, port, 10000);
    if (conn < 0) {
        printf("Connection failed\n");
        return -1;
    }

    /* Send HTTP request */
    char req[512];
    int rlen = 0;
    memcpy(req, "GET ", 4); rlen += 4;
    memcpy(req + rlen, path, plen); rlen += plen;
    memcpy(req + rlen, " HTTP/1.0\r\nHost: ", 17); rlen += 17;
    memcpy(req + rlen, host, hlen); rlen += hlen;
    memcpy(req + rlen, "\r\nConnection: close\r\n\r\n", 23); rlen += 23;

    tcp_send(conn, req, (uint16_t)rlen);

    /* Receive response */
    page_len = 0;
    char tmp[512];
    while (page_len < MAX_PAGE_SIZE - 1) {
        int n = tcp_recv(conn, tmp, sizeof(tmp), 10000);
        if (n <= 0) break;
        int space = MAX_PAGE_SIZE - 1 - page_len;
        int copy = n < space ? n : space;
        memcpy(page_buf + page_len, tmp, copy);
        page_len += copy;
    }
    page_buf[page_len] = '\0';
    tcp_close(conn);

    /* Skip HTTP headers (find \r\n\r\n) */
    char *body = NULL;
    for (int i = 0; i < page_len - 3; i++) {
        if (page_buf[i] == '\r' && page_buf[i+1] == '\n' &&
            page_buf[i+2] == '\r' && page_buf[i+3] == '\n') {
            body = page_buf + i + 4;
            break;
        }
    }
    if (!body) body = page_buf;

    /* Move body to start of page_buf */
    int body_len = page_len - (int)(body - page_buf);
    memmove(page_buf, body, body_len);
    page_buf[body_len] = '\0';
    page_len = body_len;

    return 0;
}

/* --- HTML to Text Renderer --- */

/* Simple case-insensitive compare for tag matching */
static int tag_eq(const char *tag, int taglen, const char *name) {
    int nlen = strlen(name);
    if (taglen != nlen) return 0;
    for (int i = 0; i < nlen; i++) {
        char c = tag[i];
        if (c >= 'A' && c <= 'Z') c += 32;
        if (c != name[i]) return 0;
    }
    return 1;
}

static void render_page(void) {
    int cols = terminal_get_cols();
    if (cols > MAX_LINE_LEN - 1) cols = MAX_LINE_LEN - 1;

    render_len = 0;
    line_count = 0;
    link_count = 0;

    int col = 0;           /* Current column position */
    int in_tag = 0;        /* Inside < > */
    int in_script = 0;     /* Inside <script> */
    int in_style = 0;      /* Inside <style> */
    int skip_whitespace = 1;
    int in_href = 0;       /* Collecting href attribute value */
    char href_buf[MAX_URL_LEN];
    int href_pos = 0;
    int need_newline = 0;  /* Pending line break */
    int in_pre = 0;        /* Preformatted mode */

    char tag_buf[64];      /* Current tag name */
    int tag_pos = 0;
    int tag_is_close = 0;

    /* Start first line */
    line_offsets[0] = 0;
    line_count = 1;

    const char *p = page_buf;
    const char *end = page_buf + page_len;

    while (p < end && line_count < MAX_RENDER_LINES - 1) {
        char c = *p++;

        if (in_tag) {
            if (c == '>') {
                in_tag = 0;
                tag_buf[tag_pos] = '\0';

                /* Check what tag this is */
                char *tname = tag_buf;
                tag_is_close = 0;
                if (*tname == '/') { tag_is_close = 1; tname++; }

                /* Strip attributes: find first space */
                int tlen = 0;
                while (tname[tlen] && tname[tlen] != ' ' && tname[tlen] != '\t')
                    tlen++;

                if (tag_eq(tname, tlen, "br") || tag_eq(tname, tlen, "br/")) {
                    need_newline = 1;
                } else if (tag_eq(tname, tlen, "p") || tag_eq(tname, tlen, "div") ||
                           tag_eq(tname, tlen, "h1") || tag_eq(tname, tlen, "h2") ||
                           tag_eq(tname, tlen, "h3") || tag_eq(tname, tlen, "h4") ||
                           tag_eq(tname, tlen, "tr") || tag_eq(tname, tlen, "li")) {
                    need_newline = 1;
                    if (tag_eq(tname, tlen, "li") && !tag_is_close) {
                        /* Add bullet */
                        if (need_newline && col > 0) {
                            render_buf[render_len++] = '\n';
                            line_offsets[line_count++] = render_len;
                            col = 0;
                        }
                        need_newline = 0;
                        render_buf[render_len++] = ' ';
                        render_buf[render_len++] = '*';
                        render_buf[render_len++] = ' ';
                        col += 3;
                    }
                } else if (tag_eq(tname, tlen, "script")) {
                    in_script = tag_is_close ? 0 : 1;
                } else if (tag_eq(tname, tlen, "style")) {
                    in_style = tag_is_close ? 0 : 1;
                } else if (tag_eq(tname, tlen, "pre")) {
                    in_pre = tag_is_close ? 0 : 1;
                    need_newline = 1;
                } else if (tag_eq(tname, tlen, "a") && !tag_is_close) {
                    /* Extract href from the tag */
                    char *attr = tname + tlen;
                    while (*attr == ' ' || *attr == '\t') attr++;
                    /* Look for href=" */
                    char *h = attr;
                    while (*h) {
                        if (h[0]=='h' && h[1]=='r' && h[2]=='e' && h[3]=='f' && h[4]=='=') {
                            h += 5;
                            if (*h == '"' || *h == '\'') h++;
                            href_pos = 0;
                            while (*h && *h != '"' && *h != '\'' && *h != '>' &&
                                   href_pos < MAX_URL_LEN - 1)
                                href_buf[href_pos++] = *h++;
                            href_buf[href_pos] = '\0';
                            in_href = 1;

                            /* Print link marker */
                            if (link_count < MAX_LINKS) {
                                char marker[8];
                                int mi = 0;
                                marker[mi++] = '[';
                                int lnum = link_count + 1;
                                if (lnum >= 10) marker[mi++] = '0' + (lnum / 10);
                                marker[mi++] = '0' + (lnum % 10);
                                marker[mi++] = ']';
                                for (int j = 0; j < mi && col < cols; j++) {
                                    render_buf[render_len++] = marker[j];
                                    col++;
                                }
                            }
                            break;
                        }
                        h++;
                    }
                } else if (tag_eq(tname, tlen, "a") && tag_is_close) {
                    if (in_href && link_count < MAX_LINKS) {
                        resolve_relative_url(href_buf, links[link_count].url, MAX_URL_LEN);
                        links[link_count].line = line_count - 1;
                        link_count++;
                    }
                    in_href = 0;
                }

                skip_whitespace = 1;
            } else {
                if (tag_pos < 63)
                    tag_buf[tag_pos++] = c;
            }
            continue;
        }

        /* Check for tag start */
        if (c == '<') {
            in_tag = 1;
            tag_pos = 0;
            continue;
        }

        /* Skip content inside script/style */
        if (in_script || in_style)
            continue;

        /* Handle HTML entities */
        if (c == '&' && p < end) {
            if (p[0] == 'l' && p[1] == 't' && p[2] == ';') { c = '<'; p += 3; }
            else if (p[0] == 'g' && p[1] == 't' && p[2] == ';') { c = '>'; p += 3; }
            else if (p[0] == 'a' && p[1] == 'm' && p[2] == 'p' && p[3] == ';') { c = '&'; p += 4; }
            else if (p[0] == 'q' && p[1] == 'u' && p[2] == 'o' && p[3] == 't' && p[4] == ';') { c = '"'; p += 5; }
            else if (p[0] == 'n' && p[1] == 'b' && p[2] == 's' && p[3] == 'p' && p[4] == ';') { c = ' '; p += 5; }
            else if (p[0] == '#') {
                /* Numeric entity — skip it */
                while (p < end && *p != ';') p++;
                if (p < end) p++;
                c = ' ';
            } else {
                /* Unknown entity — skip to ; */
                const char *scan = p;
                while (scan < end && *scan != ';' && (scan - p) < 10) scan++;
                if (scan < end && *scan == ';') p = scan + 1;
                c = ' ';
            }
        }

        /* Whitespace handling */
        if (!in_pre && (c == '\n' || c == '\r' || c == '\t'))
            c = ' ';

        if (!in_pre && c == ' ' && skip_whitespace)
            continue;

        /* Apply pending newline */
        if (need_newline && col > 0) {
            render_buf[render_len++] = '\n';
            if (line_count < MAX_RENDER_LINES)
                line_offsets[line_count++] = render_len;
            col = 0;
            skip_whitespace = 1;
            need_newline = 0;
            if (c == ' ') continue;
        }
        need_newline = 0;

        /* Word wrap */
        if (col >= cols) {
            render_buf[render_len++] = '\n';
            if (line_count < MAX_RENDER_LINES)
                line_offsets[line_count++] = render_len;
            col = 0;
        }

        /* Output character */
        if (c == '\n' && in_pre) {
            render_buf[render_len++] = '\n';
            if (line_count < MAX_RENDER_LINES)
                line_offsets[line_count++] = render_len;
            col = 0;
            skip_whitespace = 0;
        } else {
            render_buf[render_len++] = c;
            col++;
            skip_whitespace = (c == ' ');
        }
    }

    /* Ensure buffer is terminated */
    render_buf[render_len] = '\0';
}

/* --- Display --- */

static void display_page(int scroll_pos) {
    int rows = terminal_get_rows();
    int display_rows = rows - 2;  /* Reserve 2 lines for status/input */

    /* Clear screen */
    printf("\033[2J\033[H");

    /* Print lines from scroll_pos */
    for (int i = 0; i < display_rows && (scroll_pos + i) < line_count; i++) {
        int line_idx = scroll_pos + i;
        int start = line_offsets[line_idx];
        int end_off;
        if (line_idx + 1 < line_count)
            end_off = line_offsets[line_idx + 1];
        else
            end_off = render_len;

        /* Print line content (skip trailing \n) */
        for (int j = start; j < end_off; j++) {
            char ch = render_buf[j];
            if (ch == '\n') break;
            putchar(ch);
        }
        putchar('\n');
    }

    /* Status bar */
    change_colour_current(0, 7);  /* Black on light grey */
    int cols = terminal_get_cols();
    printf(" %s", current_url);
    int url_len = strlen(current_url) + 1;
    for (int i = url_len; i < cols; i++) putchar(' ');
    change_colour_current(7, 0);  /* White on black */

    /* Command line */
    printf("[q]uit [b]ack [j/k]scroll [g]o [number]link> ");
}

/* --- Main Browser Loop --- */

static void browser_loop(const char *start_url) {
    int scroll_pos = 0;

    /* Push to history */
    int ulen = strlen(start_url);
    if (ulen >= MAX_URL_LEN) ulen = MAX_URL_LEN - 1;
    memcpy(history[0], start_url, ulen);
    history[0][ulen] = '\0';
    history_pos = 0;
    history_count = 1;

    printf("Loading %s...\n", start_url);
    if (fetch_page(start_url) != 0)
        return;

    render_page();
    display_page(0);

    int rows = terminal_get_rows();
    int display_rows = rows - 2;

    while (1) {
        uint8_t key = get_key();

        if (key == 'q' || key == 'Q')
            break;

        if (key == 'j' || key == ' ') {
            /* Scroll down */
            if (scroll_pos + display_rows < line_count) {
                scroll_pos += display_rows / 2;
                if (scroll_pos + display_rows > line_count)
                    scroll_pos = line_count - display_rows;
                if (scroll_pos < 0) scroll_pos = 0;
            }
            display_page(scroll_pos);
        } else if (key == 'k') {
            /* Scroll up */
            scroll_pos -= display_rows / 2;
            if (scroll_pos < 0) scroll_pos = 0;
            display_page(scroll_pos);
        } else if (key == 'b' || key == 'B') {
            /* Go back */
            if (history_pos > 0) {
                history_pos--;
                printf("\nLoading %s...\n", history[history_pos]);
                if (fetch_page(history[history_pos]) == 0) {
                    render_page();
                    scroll_pos = 0;
                    display_page(0);
                }
            }
        } else if (key == 'g' || key == 'G') {
            /* Go to URL — read from keyboard */
            printf("\nURL: ");
            char url_input[MAX_URL_LEN];
            int ui = 0;
            while (ui < MAX_URL_LEN - 1) {
                uint8_t ch = get_key();
                if (ch == 0x0D) break;  /* Enter */
                if (ch == 0x08) {       /* Backspace */
                    if (ui > 0) { ui--; putchar(0x08); }
                    continue;
                }
                if (ch >= 0x20 && ch < 0x7F) {
                    url_input[ui++] = ch;
                    putchar(ch);
                }
            }
            url_input[ui] = '\0';
            printf("\n");

            if (ui > 0) {
                /* Prepend http:// if missing */
                char full_url[MAX_URL_LEN];
                if (url_input[0] != 'h') {
                    memcpy(full_url, "http://", 7);
                    memcpy(full_url + 7, url_input, ui + 1);
                } else {
                    memcpy(full_url, url_input, ui + 1);
                }

                printf("Loading %s...\n", full_url);
                if (fetch_page(full_url) == 0) {
                    /* Push to history */
                    if (history_count < MAX_HISTORY) {
                        history_pos = history_count;
                        int fl = strlen(full_url);
                        memcpy(history[history_count], full_url, fl + 1);
                        history_count++;
                    }
                    render_page();
                    scroll_pos = 0;
                    display_page(0);
                } else {
                    display_page(scroll_pos);
                }
            }
        } else if (key >= '0' && key <= '9') {
            /* Link number — collect digits */
            int link_num = key - '0';
            putchar(key);

            /* Wait briefly for more digits */
            uint32_t start = timer_get_ticks();
            while (timer_get_ticks() - start < 500) {
                key_event_t ev;
                if (keyboard_poll_event(&ev) && (ev.flags & 1) && ev.ascii >= '0' && ev.ascii <= '9') {
                    link_num = link_num * 10 + (ev.ascii - '0');
                    putchar(ev.ascii);
                    start = timer_get_ticks();
                }
            }

            link_num--;  /* Convert to 0-indexed */
            if (link_num >= 0 && link_num < link_count) {
                const char *target = links[link_num].url;
                printf("\nFollowing link [%d]: %s\n", link_num + 1, target);
                printf("Loading...\n");
                if (fetch_page(target) == 0) {
                    /* Push to history */
                    if (history_count < MAX_HISTORY) {
                        history_pos = history_count;
                        int tl = strlen(target);
                        if (tl >= MAX_URL_LEN) tl = MAX_URL_LEN - 1;
                        memcpy(history[history_count], target, tl);
                        history[history_count][tl] = '\0';
                        history_count++;
                    }
                    render_page();
                    scroll_pos = 0;
                    display_page(0);
                } else {
                    display_page(scroll_pos);
                }
            }
        }
    }

    /* Restore terminal */
    printf("\033[2J\033[H");
    change_colour_current(7, 0);
}

/* --- Shell Command --- */

void browse_cmd(void) {
    if (argc < 2) {
        printf("Usage: browse <url>\n");
        printf("  e.g.: browse http://example.com\n");
        printf("Controls: j/k=scroll, g=goto, 1-9=follow link, b=back, q=quit\n");
        return;
    }

    const char *url = argv[1];
    char full_url[MAX_URL_LEN];

    /* Prepend http:// if missing */
    if (!(url[0] == 'h' && url[1] == 't' && url[2] == 't' && url[3] == 'p')) {
        memcpy(full_url, "http://", 7);
        int ul = strlen(url);
        if (ul > MAX_URL_LEN - 8) ul = MAX_URL_LEN - 8;
        memcpy(full_url + 7, url, ul);
        full_url[7 + ul] = '\0';
    } else {
        int ul = strlen(url);
        if (ul >= MAX_URL_LEN) ul = MAX_URL_LEN - 1;
        memcpy(full_url, url, ul);
        full_url[ul] = '\0';
    }

    browser_loop(full_url);
}
