/*
 * gui — MOSS i3-style Tiling Window Manager
 *
 * Keys (Mod = Alt):
 *   Mod+Enter       — spawn new terminal
 *   Mod+Shift+Q     — close focused window
 *   Mod+J / Mod+K   — focus next / previous window
 *   Mod+H / Mod+V   — next split horizontal / vertical
 *   Mod+Shift+E     — exit WM
 *
 * All windows are terminal emulators running /bin/bash.
 * Built against musl libc for full POSIX (fork, pipe, poll, waitpid).
 */

#include <stdint.h>
#include <string.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <poll.h>
#include <fcntl.h>
#include <signal.h>

/* ================================================================== */
/*  MOSS-specific syscall wrappers                                     */
/* ================================================================== */

static inline int32_t _moss_syscall0(uint32_t num) {
    int32_t ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(num) : "memory");
    return ret;
}

static inline int32_t _moss_syscall1(uint32_t num, uint32_t a1) {
    int32_t ret;
    asm volatile("int $0x80" : "=a"(ret) : "a"(num), "b"(a1) : "memory");
    return ret;
}

#define SYS_FBMAP          500
#define SYS_FB_FLUSH       506
#define SYS_POLL_KEY       503
#define SYS_GET_MOUSE_POS  514

typedef struct {
    uint32_t address;
    uint32_t width;
    uint32_t height;
    uint32_t pitch;
    uint8_t  bpp;
    uint8_t  red_pos, red_size;
    uint8_t  green_pos, green_size;
    uint8_t  blue_pos, blue_size;
} __attribute__((packed)) moss_fbinfo_t;

typedef struct {
    uint8_t scancode;
    uint8_t flags;
    uint8_t ascii;
    uint8_t modifiers;
} key_event_t;

#define KEY_EVENT_PRESS    0x01
#define KEY_EVENT_RELEASE  0x02
#define KEY_EVENT_EXTENDED 0x04
#define KEY_MOD_SHIFT      0x01
#define KEY_MOD_CTRL       0x02
#define KEY_MOD_ALT        0x04

static uint32_t moss_fb_map(moss_fbinfo_t *info) {
    return (uint32_t)_moss_syscall1(SYS_FBMAP, (uint32_t)info);
}
static void moss_fb_flush(void) {
    _moss_syscall0(SYS_FB_FLUSH);
}
static int moss_poll_key(key_event_t *ev) {
    return (int)_moss_syscall1(SYS_POLL_KEY, (uint32_t)ev);
}
static int moss_get_mouse_pos(int32_t *x, int32_t *y) {
    int32_t pos[2];
    int ret = (int)_moss_syscall1(SYS_GET_MOUSE_POS, (uint32_t)pos);
    if (x) *x = pos[0];
    if (y) *y = pos[1];
    return ret;
}

/* ================================================================== */
/*  Framebuffer globals                                                */
/* ================================================================== */

static moss_fbinfo_t fb;
static uint32_t *framebuffer;
static uint32_t screen_w, screen_h, pitch;

/* ================================================================== */
/*  Colors                                                             */
/* ================================================================== */

static inline uint32_t rgb(uint8_t r, uint8_t g, uint8_t b) {
    return ((uint32_t)r << fb.red_pos) |
           ((uint32_t)g << fb.green_pos) |
           ((uint32_t)b << fb.blue_pos);
}

/* i3 default colors */
#define COL_BG_R       40
#define COL_BG_G       40
#define COL_BG_B       40
#define COL_BORDER_FOCUSED_R  77
#define COL_BORDER_FOCUSED_G 120
#define COL_BORDER_FOCUSED_B 204
#define COL_BORDER_UNFOCUSED_R 51
#define COL_BORDER_UNFOCUSED_G 51
#define COL_BORDER_UNFOCUSED_B 51
#define COL_BAR_BG_R   30
#define COL_BAR_BG_G   30
#define COL_BAR_BG_B   30
#define COL_BAR_WS_ACTIVE_R  40
#define COL_BAR_WS_ACTIVE_G  80
#define COL_BAR_WS_ACTIVE_B 140

/* ================================================================== */
/*  Drawing primitives                                                 */
/* ================================================================== */

static inline void put_pixel(int x, int y, uint32_t color) {
    if (x >= 0 && x < (int)screen_w && y >= 0 && y < (int)screen_h)
        framebuffer[y * (pitch / 4) + x] = color;
}

static void fill_rect(int x, int y, int w, int h, uint32_t color) {
    int x0 = x < 0 ? 0 : x;
    int y0 = y < 0 ? 0 : y;
    int x1 = (x + w) > (int)screen_w ? (int)screen_w : (x + w);
    int y1 = (y + h) > (int)screen_h ? (int)screen_h : (y + h);
    uint32_t stride = pitch / 4;
    for (int row = y0; row < y1; row++) {
        uint32_t *dst = &framebuffer[row * stride + x0];
        for (int col = x0; col < x1; col++)
            *dst++ = color;
    }
}

/* ================================================================== */
/*  Bitmap font (8x8)                                                  */
/* ================================================================== */

static const uint8_t font8x8[][8] = {
    [' '-32] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00},
    ['!'-32] = {0x18,0x18,0x18,0x18,0x18,0x00,0x18,0x00},
    ['"'-32] = {0x6C,0x6C,0x00,0x00,0x00,0x00,0x00,0x00},
    ['#'-32] = {0x6C,0x6C,0xFE,0x6C,0xFE,0x6C,0x6C,0x00},
    ['$'-32] = {0x18,0x7E,0xC0,0x7C,0x06,0xFC,0x18,0x00},
    ['%'-32] = {0x00,0xC6,0xCC,0x18,0x30,0x66,0xC6,0x00},
    ['&'-32] = {0x38,0x6C,0x38,0x76,0xDC,0xCC,0x76,0x00},
    ['\''-32]= {0x18,0x18,0x30,0x00,0x00,0x00,0x00,0x00},
    ['('-32] = {0x0C,0x18,0x30,0x30,0x30,0x18,0x0C,0x00},
    [')'-32] = {0x30,0x18,0x0C,0x0C,0x0C,0x18,0x30,0x00},
    ['*'-32] = {0x00,0x66,0x3C,0xFF,0x3C,0x66,0x00,0x00},
    ['+'-32] = {0x00,0x18,0x18,0x7E,0x18,0x18,0x00,0x00},
    [','-32] = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x30},
    ['-'-32] = {0x00,0x00,0x00,0x7E,0x00,0x00,0x00,0x00},
    ['.'-32] = {0x00,0x00,0x00,0x00,0x00,0x18,0x18,0x00},
    ['/'-32] = {0x06,0x0C,0x18,0x30,0x60,0xC0,0x80,0x00},
    ['0'-32] = {0x7C,0xC6,0xCE,0xDE,0xF6,0xE6,0x7C,0x00},
    ['1'-32] = {0x18,0x38,0x78,0x18,0x18,0x18,0x7E,0x00},
    ['2'-32] = {0x7C,0xC6,0x06,0x1C,0x30,0x66,0xFE,0x00},
    ['3'-32] = {0x7C,0xC6,0x06,0x3C,0x06,0xC6,0x7C,0x00},
    ['4'-32] = {0x1C,0x3C,0x6C,0xCC,0xFE,0x0C,0x1E,0x00},
    ['5'-32] = {0xFE,0xC0,0xC0,0xFC,0x06,0xC6,0x7C,0x00},
    ['6'-32] = {0x38,0x60,0xC0,0xFC,0xC6,0xC6,0x7C,0x00},
    ['7'-32] = {0xFE,0xC6,0x0C,0x18,0x30,0x30,0x30,0x00},
    ['8'-32] = {0x7C,0xC6,0xC6,0x7C,0xC6,0xC6,0x7C,0x00},
    ['9'-32] = {0x7C,0xC6,0xC6,0x7E,0x06,0x0C,0x78,0x00},
    [':'-32] = {0x00,0x18,0x18,0x00,0x18,0x18,0x00,0x00},
    [';'-32] = {0x00,0x18,0x18,0x00,0x18,0x18,0x30,0x00},
    ['<'-32] = {0x06,0x0C,0x18,0x30,0x18,0x0C,0x06,0x00},
    ['='-32] = {0x00,0x00,0x7E,0x00,0x7E,0x00,0x00,0x00},
    ['>'-32] = {0x60,0x30,0x18,0x0C,0x18,0x30,0x60,0x00},
    ['?'-32] = {0x7C,0xC6,0x0C,0x18,0x18,0x00,0x18,0x00},
    ['@'-32] = {0x7C,0xC6,0xDE,0xDE,0xDE,0xC0,0x78,0x00},
    ['A'-32] = {0x38,0x6C,0xC6,0xC6,0xFE,0xC6,0xC6,0x00},
    ['B'-32] = {0xFC,0x66,0x66,0x7C,0x66,0x66,0xFC,0x00},
    ['C'-32] = {0x3C,0x66,0xC0,0xC0,0xC0,0x66,0x3C,0x00},
    ['D'-32] = {0xF8,0x6C,0x66,0x66,0x66,0x6C,0xF8,0x00},
    ['E'-32] = {0xFE,0x62,0x68,0x78,0x68,0x62,0xFE,0x00},
    ['F'-32] = {0xFE,0x62,0x68,0x78,0x68,0x60,0xF0,0x00},
    ['G'-32] = {0x3C,0x66,0xC0,0xC0,0xCE,0x66,0x3E,0x00},
    ['H'-32] = {0xC6,0xC6,0xC6,0xFE,0xC6,0xC6,0xC6,0x00},
    ['I'-32] = {0x3C,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    ['J'-32] = {0x1E,0x0C,0x0C,0x0C,0xCC,0xCC,0x78,0x00},
    ['K'-32] = {0xE6,0x66,0x6C,0x78,0x6C,0x66,0xE6,0x00},
    ['L'-32] = {0xF0,0x60,0x60,0x60,0x62,0x66,0xFE,0x00},
    ['M'-32] = {0xC6,0xEE,0xFE,0xD6,0xC6,0xC6,0xC6,0x00},
    ['N'-32] = {0xC6,0xE6,0xF6,0xDE,0xCE,0xC6,0xC6,0x00},
    ['O'-32] = {0x7C,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    ['P'-32] = {0xFC,0x66,0x66,0x7C,0x60,0x60,0xF0,0x00},
    ['Q'-32] = {0x7C,0xC6,0xC6,0xC6,0xD6,0xDE,0x7C,0x06},
    ['R'-32] = {0xFC,0x66,0x66,0x7C,0x6C,0x66,0xE6,0x00},
    ['S'-32] = {0x7C,0xC6,0xC0,0x7C,0x06,0xC6,0x7C,0x00},
    ['T'-32] = {0x7E,0x5A,0x18,0x18,0x18,0x18,0x3C,0x00},
    ['U'-32] = {0xC6,0xC6,0xC6,0xC6,0xC6,0xC6,0x7C,0x00},
    ['V'-32] = {0xC6,0xC6,0xC6,0xC6,0x6C,0x38,0x10,0x00},
    ['W'-32] = {0xC6,0xC6,0xC6,0xD6,0xFE,0xEE,0xC6,0x00},
    ['X'-32] = {0xC6,0x6C,0x38,0x38,0x6C,0xC6,0xC6,0x00},
    ['Y'-32] = {0x66,0x66,0x66,0x3C,0x18,0x18,0x3C,0x00},
    ['Z'-32] = {0xFE,0xC6,0x8C,0x18,0x32,0x66,0xFE,0x00},
    ['['-32] = {0x3C,0x30,0x30,0x30,0x30,0x30,0x3C,0x00},
    ['\\'-32]= {0xC0,0x60,0x30,0x18,0x0C,0x06,0x02,0x00},
    [']'-32] = {0x3C,0x0C,0x0C,0x0C,0x0C,0x0C,0x3C,0x00},
    ['^'-32] = {0x10,0x38,0x6C,0xC6,0x00,0x00,0x00,0x00},
    ['_'-32] = {0x00,0x00,0x00,0x00,0x00,0x00,0x00,0xFF},
    ['`'-32] = {0x30,0x18,0x0C,0x00,0x00,0x00,0x00,0x00},
    ['a'-32] = {0x00,0x00,0x78,0x0C,0x7C,0xCC,0x76,0x00},
    ['b'-32] = {0xE0,0x60,0x7C,0x66,0x66,0x66,0xDC,0x00},
    ['c'-32] = {0x00,0x00,0x7C,0xC6,0xC0,0xC6,0x7C,0x00},
    ['d'-32] = {0x1C,0x0C,0x7C,0xCC,0xCC,0xCC,0x76,0x00},
    ['e'-32] = {0x00,0x00,0x7C,0xC6,0xFE,0xC0,0x7C,0x00},
    ['f'-32] = {0x1C,0x36,0x30,0x78,0x30,0x30,0x78,0x00},
    ['g'-32] = {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0xF8},
    ['h'-32] = {0xE0,0x60,0x6C,0x76,0x66,0x66,0xE6,0x00},
    ['i'-32] = {0x18,0x00,0x38,0x18,0x18,0x18,0x3C,0x00},
    ['j'-32] = {0x06,0x00,0x06,0x06,0x06,0x66,0x66,0x3C},
    ['k'-32] = {0xE0,0x60,0x66,0x6C,0x78,0x6C,0xE6,0x00},
    ['l'-32] = {0x38,0x18,0x18,0x18,0x18,0x18,0x3C,0x00},
    ['m'-32] = {0x00,0x00,0xEC,0xFE,0xD6,0xD6,0xD6,0x00},
    ['n'-32] = {0x00,0x00,0xDC,0x66,0x66,0x66,0x66,0x00},
    ['o'-32] = {0x00,0x00,0x7C,0xC6,0xC6,0xC6,0x7C,0x00},
    ['p'-32] = {0x00,0x00,0xDC,0x66,0x66,0x7C,0x60,0xF0},
    ['q'-32] = {0x00,0x00,0x76,0xCC,0xCC,0x7C,0x0C,0x1E},
    ['r'-32] = {0x00,0x00,0xDC,0x76,0x60,0x60,0xF0,0x00},
    ['s'-32] = {0x00,0x00,0x7C,0xC0,0x7C,0x06,0xFC,0x00},
    ['t'-32] = {0x30,0x30,0x7C,0x30,0x30,0x36,0x1C,0x00},
    ['u'-32] = {0x00,0x00,0xCC,0xCC,0xCC,0xCC,0x76,0x00},
    ['v'-32] = {0x00,0x00,0xC6,0xC6,0xC6,0x6C,0x38,0x00},
    ['w'-32] = {0x00,0x00,0xC6,0xD6,0xD6,0xFE,0x6C,0x00},
    ['x'-32] = {0x00,0x00,0xC6,0x6C,0x38,0x6C,0xC6,0x00},
    ['y'-32] = {0x00,0x00,0xC6,0xC6,0xCE,0x76,0x06,0xFC},
    ['z'-32] = {0x00,0x00,0xFC,0x98,0x30,0x64,0xFC,0x00},
    ['{'-32] = {0x0E,0x18,0x18,0x70,0x18,0x18,0x0E,0x00},
    ['|'-32] = {0x18,0x18,0x18,0x00,0x18,0x18,0x18,0x00},
    ['}'-32] = {0x70,0x18,0x18,0x0E,0x18,0x18,0x70,0x00},
    ['~'-32] = {0x76,0xDC,0x00,0x00,0x00,0x00,0x00,0x00},
};

static void draw_char(int x, int y, char c, uint32_t color) {
    if (c < 32 || c > 126) return;
    const uint8_t *glyph = font8x8[c - 32];
    for (int row = 0; row < 8; row++) {
        uint8_t bits = glyph[row];
        for (int col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))
                put_pixel(x + col, y + row, color);
        }
    }
}

static void draw_text(int x, int y, const char *text, uint32_t color) {
    while (*text) {
        draw_char(x, y, *text, color);
        x += 8;
        text++;
    }
}

static int text_len(const char *s) {
    int n = 0;
    while (s[n]) n++;
    return n;
}

/* ================================================================== */
/*  Terminal state (per-window)                                         */
/* ================================================================== */

#define MAX_TILES     8
#define TERM_INPUT_MAX 256

typedef struct {
    /* Geometry (set by tiling layout) */
    int x, y, w, h;

    /* Terminal buffer — dynamically sized based on tile geometry */
    int cols, rows;
    char screen[80][256];   /* up to 80 rows x 255 cols */
    int cur_row, cur_col;

    /* Input */
    char input[TERM_INPUT_MAX];
    int input_len;

    /* Shell process */
    int shell_fd;     /* read from shell stdout */
    int stdin_fd;     /* write to shell stdin */
    pid_t shell_pid;
    int shell_alive;

    int active;  /* 1 = in use */
} tile_t;

static tile_t tiles[MAX_TILES];
static int tile_count = 0;
static int focused_tile = -1;

/* ================================================================== */
/*  Terminal operations                                                 */
/* ================================================================== */

static void tile_clear(tile_t *t) {
    for (int r = 0; r < t->rows; r++)
        memset(t->screen[r], 0, t->cols + 1);
    t->cur_row = 0;
    t->cur_col = 0;
}

static void tile_scroll(tile_t *t) {
    for (int r = 0; r < t->rows - 1; r++)
        memcpy(t->screen[r], t->screen[r + 1], t->cols + 1);
    memset(t->screen[t->rows - 1], 0, t->cols + 1);
    t->cur_row = t->rows - 1;
}

static void tile_putchar(tile_t *t, char c) {
    if (c == '\n' || c == '\r') {
        t->cur_col = 0;
        t->cur_row++;
        if (t->cur_row >= t->rows)
            tile_scroll(t);
        return;
    }
    if (c == '\t') {
        int next = (t->cur_col + 4) & ~3;
        if (next > t->cols) next = t->cols;
        while (t->cur_col < next)
            t->screen[t->cur_row][t->cur_col++] = ' ';
        return;
    }
    if (c == '\b') {
        if (t->cur_col > 0) t->cur_col--;
        return;
    }
    if (c < 32 || c > 126) return;

    if (t->cur_col >= t->cols) {
        t->cur_col = 0;
        t->cur_row++;
        if (t->cur_row >= t->rows)
            tile_scroll(t);
    }
    t->screen[t->cur_row][t->cur_col++] = c;
}

static void tile_puts(tile_t *t, const char *s, int len) {
    for (int i = 0; i < len; i++)
        tile_putchar(t, s[i]);
}

static void tile_start_shell(tile_t *t) {
    int pipe_in[2], pipe_out[2];
    if (pipe(pipe_in) < 0 || pipe(pipe_out) < 0) {
        tile_puts(t, "pipe() failed\n", 14);
        return;
    }

    pid_t pid = fork();
    if (pid < 0) {
        tile_puts(t, "fork() failed\n", 14);
        close(pipe_in[0]); close(pipe_in[1]);
        close(pipe_out[0]); close(pipe_out[1]);
        return;
    }

    if (pid == 0) {
        close(pipe_in[1]);
        close(pipe_out[0]);
        dup2(pipe_in[0], 0);
        dup2(pipe_out[1], 1);
        dup2(pipe_out[1], 2);
        close(pipe_in[0]);
        close(pipe_out[1]);
        char *argv[] = { "bash", "--norc", "--noprofile", NULL };
        execve("/bin/bash", argv, NULL);
        argv[0] = "sh"; argv[1] = NULL;
        execve("/bin/sh", argv, NULL);
        _exit(127);
    }

    close(pipe_in[0]);
    close(pipe_out[1]);
    t->shell_fd = pipe_out[0];
    t->stdin_fd = pipe_in[1];
    t->shell_pid = pid;
    t->shell_alive = 1;
}

static void tile_read_output(tile_t *t) {
    if (!t->shell_alive || t->shell_fd < 0) return;

    struct pollfd pfd;
    pfd.fd = t->shell_fd;
    pfd.events = POLLIN;
    pfd.revents = 0;

    int ret = poll(&pfd, 1, 0);
    if (ret > 0 && (pfd.revents & POLLIN)) {
        char buf[512];
        ssize_t n = read(t->shell_fd, buf, sizeof(buf));
        if (n > 0) {
            tile_puts(t, buf, (int)n);
        } else if (n == 0) {
            t->shell_alive = 0;
            close(t->shell_fd);
            close(t->stdin_fd);
            t->shell_fd = -1;
            t->stdin_fd = -1;
            waitpid(t->shell_pid, NULL, 0);
            tile_puts(t, "[exited]\n", 9);
        }
    }
}

static void tile_send_input(tile_t *t) {
    if (!t->shell_alive || t->stdin_fd < 0) return;
    if (t->input_len <= 0) return;

    tile_puts(t, t->input, t->input_len);
    tile_putchar(t, '\n');

    write(t->stdin_fd, t->input, t->input_len);
    write(t->stdin_fd, "\n", 1);
    t->input_len = 0;
}

static void tile_destroy(tile_t *t) {
    /* Close pipes first — this causes the shell's read() to return EOF/error,
     * which usually makes it exit. Then SIGKILL to be sure. */
    if (t->stdin_fd >= 0) { close(t->stdin_fd); t->stdin_fd = -1; }
    if (t->shell_fd >= 0) { close(t->shell_fd); t->shell_fd = -1; }
    if (t->shell_alive && t->shell_pid > 0) {
        kill(t->shell_pid, SIGKILL);
        waitpid(t->shell_pid, NULL, 0);
    }
    t->active = 0;
    t->shell_alive = 0;
}

/* ================================================================== */
/*  Tiling layout                                                      */
/* ================================================================== */

#define BORDER_PX  2
#define BAR_H      16
#define GAP_PX     4

/*
 * Simple i3-style master-stack tiling:
 * - 1 window: fills entire area
 * - 2+ windows: first window gets left half, rest stack on the right half
 *   (or top/bottom depending on split direction)
 *
 * For simplicity we do a "spiral" tile like i3's default:
 *   Each new window splits the space of the last window.
 */

static void recalculate_tiles(void) {
    /* Count active tiles */
    int active_indices[MAX_TILES];
    int active_count = 0;
    for (int i = 0; i < MAX_TILES; i++) {
        if (tiles[i].active)
            active_indices[active_count++] = i;
    }
    if (active_count == 0) return;

    int area_x = GAP_PX;
    int area_y = GAP_PX;
    int area_w = (int)screen_w - GAP_PX * 2;
    int area_h = (int)screen_h - BAR_H - GAP_PX * 2;

    if (active_count == 1) {
        int idx = active_indices[0];
        tiles[idx].x = area_x;
        tiles[idx].y = area_y;
        tiles[idx].w = area_w;
        tiles[idx].h = area_h;
    } else {
        /* Master-stack layout: first window gets left half,
         * remaining windows split the right half evenly */
        int master_w = area_w / 2;
        int stack_w = area_w - master_w - GAP_PX;

        /* Master */
        int mi = active_indices[0];
        tiles[mi].x = area_x;
        tiles[mi].y = area_y;
        tiles[mi].w = master_w;
        tiles[mi].h = area_h;

        /* Stack */
        int stack_count = active_count - 1;
        int each_h = (area_h - GAP_PX * (stack_count - 1)) / stack_count;
        int sx = area_x + master_w + GAP_PX;

        for (int s = 0; s < stack_count; s++) {
            int si = active_indices[s + 1];
            tiles[si].x = sx;
            tiles[si].y = area_y + s * (each_h + GAP_PX);
            tiles[si].w = stack_w;
            tiles[si].h = each_h;
        }
    }

    /* Recalculate terminal cols/rows for each tile */
    for (int i = 0; i < active_count; i++) {
        int idx = active_indices[i];
        tile_t *t = &tiles[idx];
        int content_w = t->w - BORDER_PX * 2;
        int content_h = t->h - BORDER_PX * 2;
        int new_cols = content_w / 8;
        int new_rows = content_h / 10;
        if (new_cols < 10) new_cols = 10;
        if (new_rows < 3) new_rows = 3;
        if (new_cols > 255) new_cols = 255;
        if (new_rows > 80) new_rows = 80;
        t->cols = new_cols;
        t->rows = new_rows;
        /* Clamp cursor */
        if (t->cur_col >= t->cols) t->cur_col = t->cols - 1;
        if (t->cur_row >= t->rows) t->cur_row = t->rows - 1;
    }
}

static int spawn_tile(void) {
    /* Find free slot */
    int idx = -1;
    for (int i = 0; i < MAX_TILES; i++) {
        if (!tiles[i].active) { idx = i; break; }
    }
    if (idx < 0) return -1;

    tile_t *t = &tiles[idx];
    memset(t, 0, sizeof(*t));
    t->active = 1;
    t->shell_fd = -1;
    t->stdin_fd = -1;
    t->shell_pid = -1;
    t->cols = 80;
    t->rows = 24;
    tile_count++;

    recalculate_tiles();
    tile_clear(t);
    tile_start_shell(t);

    focused_tile = idx;
    return idx;
}

static void close_tile(int idx) {
    if (idx < 0 || idx >= MAX_TILES || !tiles[idx].active) return;
    tile_destroy(&tiles[idx]);
    tile_count--;

    /* Move focus */
    if (focused_tile == idx) {
        focused_tile = -1;
        for (int i = 0; i < MAX_TILES; i++) {
            if (tiles[i].active) { focused_tile = i; break; }
        }
    }
    recalculate_tiles();
}

static void focus_next(void) {
    if (tile_count <= 1) return;
    int start = focused_tile;
    int i = (start + 1) % MAX_TILES;
    while (i != start) {
        if (tiles[i].active) { focused_tile = i; return; }
        i = (i + 1) % MAX_TILES;
    }
}

static void focus_prev(void) {
    if (tile_count <= 1) return;
    int start = focused_tile;
    int i = (start - 1 + MAX_TILES) % MAX_TILES;
    while (i != start) {
        if (tiles[i].active) { focused_tile = i; return; }
        i = (i - 1 + MAX_TILES) % MAX_TILES;
    }
}

/* ================================================================== */
/*  Rendering                                                          */
/* ================================================================== */

static void render_tile(tile_t *t, int is_focused) {
    /* Border color */
    uint32_t border_col = is_focused
        ? rgb(COL_BORDER_FOCUSED_R, COL_BORDER_FOCUSED_G, COL_BORDER_FOCUSED_B)
        : rgb(COL_BORDER_UNFOCUSED_R, COL_BORDER_UNFOCUSED_G, COL_BORDER_UNFOCUSED_B);

    /* Draw border */
    fill_rect(t->x, t->y, t->w, BORDER_PX, border_col);                         /* top */
    fill_rect(t->x, t->y + t->h - BORDER_PX, t->w, BORDER_PX, border_col);      /* bottom */
    fill_rect(t->x, t->y, BORDER_PX, t->h, border_col);                          /* left */
    fill_rect(t->x + t->w - BORDER_PX, t->y, BORDER_PX, t->h, border_col);      /* right */

    /* Content area background (dark terminal) */
    int cx = t->x + BORDER_PX;
    int cy = t->y + BORDER_PX;
    int cw = t->w - BORDER_PX * 2;
    int ch = t->h - BORDER_PX * 2;
    fill_rect(cx, cy, cw, ch, rgb(20, 20, 20));

    /* Render terminal text */
    uint32_t fg = rgb(200, 200, 200);
    uint32_t input_fg = rgb(180, 220, 180);
    for (int r = 0; r < t->rows; r++) {
        for (int c = 0; c < t->cols; c++) {
            char ch2 = t->screen[r][c];
            if (ch2)
                draw_char(cx + c * 8, cy + r * 10, ch2, fg);
        }
    }

    /* Input line + cursor (only if focused and shell alive) */
    if (is_focused && t->shell_alive) {
        int input_y = cy + t->cur_row * 10;
        int input_x = cx + t->cur_col * 8;
        for (int i = 0; i < t->input_len; i++)
            draw_char(input_x + i * 8, input_y, t->input[i], input_fg);
        /* Block cursor */
        fill_rect(input_x + t->input_len * 8, input_y, 8, 10, input_fg);
    }
}

static void render_bar(void) {
    int bar_y = (int)screen_h - BAR_H;

    fill_rect(0, bar_y, (int)screen_w, BAR_H, rgb(COL_BAR_BG_R, COL_BAR_BG_G, COL_BAR_BG_B));

    /* Workspace indicators — show tile count */
    int wx = 4;
    for (int i = 0; i < MAX_TILES; i++) {
        if (!tiles[i].active) continue;
        int is_focused = (i == focused_tile);
        uint32_t ws_bg = is_focused
            ? rgb(COL_BORDER_FOCUSED_R, COL_BORDER_FOCUSED_G, COL_BORDER_FOCUSED_B)
            : rgb(COL_BORDER_UNFOCUSED_R, COL_BORDER_UNFOCUSED_G, COL_BORDER_UNFOCUSED_B);
        uint32_t ws_fg = rgb(255, 255, 255);

        fill_rect(wx, bar_y + 2, 20, BAR_H - 4, ws_bg);
        char num[2] = { '1' + (char)i, 0 };
        draw_text(wx + 6, bar_y + 4, num, ws_fg);
        wx += 24;
    }

    /* Right side: show keybind hint */
    const char *hint = "Alt+Enter:new  Alt+Shift+Q:close  Alt+Shift+E:exit";
    int hint_x = (int)screen_w - text_len(hint) * 8 - 4;
    draw_text(hint_x, bar_y + 4, hint, rgb(120, 120, 120));
}

/* ================================================================== */
/*  Mouse cursor                                                       */
/* ================================================================== */

static const uint8_t cursor_data[16][10] = {
    {1,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0},
    {1,2,2,2,2,2,2,1,0,0},
    {1,2,2,2,2,2,2,2,1,0},
    {1,2,2,2,2,2,1,1,1,1},
    {1,2,2,2,2,1,0,0,0,0},
    {1,2,1,1,2,2,1,0,0,0},
    {1,1,0,0,1,2,1,0,0,0},
    {1,0,0,0,1,2,2,1,0,0},
    {0,0,0,0,0,1,2,1,0,0},
    {0,0,0,0,0,1,1,1,0,0},
};

static void draw_cursor(int mx, int my) {
    for (int row = 0; row < 16; row++) {
        for (int col = 0; col < 10; col++) {
            uint8_t p = cursor_data[row][col];
            if (p == 2)
                put_pixel(mx + col, my + row, rgb(255, 255, 255));
            else if (p == 1)
                put_pixel(mx + col, my + row, rgb(0, 0, 0));
        }
    }
}

static void render_frame(int mx, int my) {
    /* Dark background (gaps) */
    fill_rect(0, 0, (int)screen_w, (int)screen_h, rgb(COL_BG_R, COL_BG_G, COL_BG_B));

    /* Tiles */
    for (int i = 0; i < MAX_TILES; i++) {
        if (!tiles[i].active) continue;
        render_tile(&tiles[i], i == focused_tile);
    }

    /* Status bar */
    render_bar();

    /* Mouse cursor on top */
    draw_cursor(mx, my);
}

/* ================================================================== */
/*  Main                                                               */
/* ================================================================== */

int main(void) {
    signal(SIGPIPE, SIG_IGN);

    uint32_t addr = moss_fb_map(&fb);
    if (!addr) return 1;

    framebuffer = (uint32_t *)(uintptr_t)addr;
    screen_w = fb.width;
    screen_h = fb.height;
    pitch = fb.pitch;

    /* Spawn initial terminal */
    spawn_tile();

    int running = 1;
    while (running) {
        /* --- Read shell output from all tiles --- */
        for (int i = 0; i < MAX_TILES; i++) {
            if (tiles[i].active)
                tile_read_output(&tiles[i]);
        }

        /* --- Keyboard input --- */
        key_event_t kev;
        while (moss_poll_key(&kev)) {
            if (!(kev.flags & KEY_EVENT_PRESS)) continue;

            int mod = (kev.modifiers & KEY_MOD_ALT);
            int shift = (kev.modifiers & KEY_MOD_SHIFT);

            if (mod) {
                /* WM keybindings (Alt+key) */
                /* Enter = scancode 0x1C */
                if (kev.scancode == 0x1C) {
                    /* Alt+Enter: spawn new terminal */
                    spawn_tile();
                    continue;
                }
                if (shift && (kev.ascii == 'Q' || kev.ascii == 'q')) {
                    /* Alt+Shift+Q: close focused tile */
                    if (focused_tile >= 0)
                        close_tile(focused_tile);
                    continue;
                }
                if (shift && (kev.ascii == 'E' || kev.ascii == 'e')) {
                    /* Alt+Shift+E: exit WM */
                    running = 0;
                    break;
                }
                if (kev.ascii == 'j' || kev.ascii == 'J') {
                    /* Alt+J: focus next */
                    focus_next();
                    continue;
                }
                if (kev.ascii == 'k' || kev.ascii == 'K') {
                    /* Alt+K: focus prev */
                    focus_prev();
                    continue;
                }
                /* Consume other Alt combos so they don't go to terminal */
                continue;
            }

            /* Route to focused terminal */
            if (focused_tile >= 0 && tiles[focused_tile].active) {
                tile_t *t = &tiles[focused_tile];
                if (t->shell_alive) {
                    if (kev.ascii == '\r' || kev.ascii == '\n') {
                        tile_send_input(t);
                    } else if (kev.ascii == '\b' || kev.ascii == 0x7F) {
                        if (t->input_len > 0)
                            t->input_len--;
                    } else if (kev.ascii >= 32 && kev.ascii < 127) {
                        if (t->input_len < TERM_INPUT_MAX - 1)
                            t->input[t->input_len++] = kev.ascii;
                    }
                }
            }
        }

        if (!running) break;
        if (tile_count == 0) break;  /* Exit if all tiles closed */

        /* --- Mouse position (for cursor rendering) --- */
        int32_t mx, my;
        moss_get_mouse_pos(&mx, &my);

        /* --- Render --- */
        render_frame((int)mx, (int)my);
        moss_fb_flush();

        usleep(16000);
    }

    /* Cleanup all tiles */
    for (int i = 0; i < MAX_TILES; i++) {
        if (tiles[i].active)
            tile_destroy(&tiles[i]);
    }

    memset(framebuffer, 0, pitch * screen_h);
    moss_fb_flush();
    return 0;
}
