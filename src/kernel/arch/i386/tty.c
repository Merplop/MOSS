#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/io.h>
#include <kernel/tty.h>

#include "framebuffer.h"
#include "font8x16.h"

/* ---- Serial port (COM1) mirror for debug logging ---- */
#define COM1_PORT 0x3F8

static int serial_inited = 0;

void serial_init(void) {
    outb(COM1_PORT + 1, 0x00); /* Disable interrupts */
    outb(COM1_PORT + 3, 0x80); /* Set DLAB (baud rate divisor) */
    outb(COM1_PORT + 0, 0x01); /* 115200 baud (divisor 1) */
    outb(COM1_PORT + 1, 0x00);
    outb(COM1_PORT + 3, 0x03); /* 8N1 */
    outb(COM1_PORT + 2, 0xC7); /* Enable FIFO, clear, 14-byte threshold */
    outb(COM1_PORT + 4, 0x0B); /* IRQs enabled, RTS/DSR set */
    serial_inited = 1;
}

static inline void serial_putc(char c) {
    if (!serial_inited) return;
    /* Wait for transmit buffer empty */
    while (!(inb(COM1_PORT + 5) & 0x20))
        ;
    outb(COM1_PORT, (uint8_t)c);
}

static void serial_write(const char *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        if (data[i] == '\n')
            serial_putc('\r');
        serial_putc(data[i]);
    }
}

/* Terminal dimensions in characters (computed from framebuffer size) */
static size_t TERM_COLS;
static size_t TERM_ROWS;

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_fg = VGA_COLOUR_LIGHT_GREY;
static uint8_t terminal_bg = VGA_COLOUR_BLACK;
static uint32_t fb_fg;
static uint32_t fb_bg;

/* Saved cursor position for ESC [s / ESC [u */
static size_t saved_row = 0;
static size_t saved_col = 0;

/* Whether the cursor is visible (for ESC [?25h / ESC [?25l) */
static bool cursor_visible = true;

/* Cursor drawing state: track where the cursor was last rendered so we can
   erase it before moving to the new position. */
static size_t cursor_draw_row = 0;
static size_t cursor_draw_col = 0;
static uint32_t cursor_draw_bg = 0;
static bool cursor_drawn = false;

#define CURSOR_HEIGHT 2   /* underline thickness in pixels */

/*
 * Deferred wrap flag — VT100-style behavior.
 * When a character is written to the last column, the cursor stays there
 * and this flag is set. The wrap (and possible scroll) only happens when
 * the NEXT character arrives. This prevents spurious scrolls when a
 * full-screen app fills the bottom-right cell.
 */
static bool deferred_wrap = false;

/* ---- ANSI/VT100 escape sequence parser ---- */

#define ANSI_MAX_PARAMS 8
#define ANSI_BUF_SIZE   32

typedef enum {
    ANSI_NORMAL,
    ANSI_ESC,       /* got ESC */
    ANSI_CSI,       /* got ESC [ */
    ANSI_CSI_QMARK  /* got ESC [ ? */
} ansi_state_t;

static ansi_state_t ansi_state = ANSI_NORMAL;
static int ansi_params[ANSI_MAX_PARAMS];
static int ansi_param_count = 0;
static int ansi_cur_param = 0;   /* value being accumulated */
static bool ansi_has_digit = false;

/* Standard CGA/VGA 16-colour palette mapped to 32-bit RGB */
static const uint32_t vga_palette_rgb[16][3] = {
	{  0,   0,   0}, /* BLACK         */
	{  0,   0, 170}, /* BLUE          */
	{  0, 170,   0}, /* GREEN         */
	{  0, 170, 170}, /* CYAN          */
	{170,   0,   0}, /* RED           */
	{170,   0, 170}, /* MAGENTA       */
	{170,  85,   0}, /* BROWN         */
	{170, 170, 170}, /* LIGHT_GREY    */
	{ 85,  85,  85}, /* DARK_GREY     */
	{ 85,  85, 255}, /* LIGHT_BLUE    */
	{ 85, 255,  85}, /* LIGHT_GREEN   */
	{ 85, 255, 255}, /* LIGHT_CYAN    */
	{255,  85,  85}, /* LIGHT_RED     */
	{255,  85, 255}, /* LIGHT_MAGENTA */
	{255, 255,  85}, /* LIGHT_BROWN   */
	{255, 255, 255}, /* WHITE         */
};

static uint32_t palette_to_fb(enum vga_colour c) {
	const uint32_t *rgb = vga_palette_rgb[c & 0x0F];
	return fb_make_color(rgb[0], rgb[1], rgb[2]);
}

static void update_fb_colours(void) {
	fb_fg = palette_to_fb(terminal_fg);
	fb_bg = palette_to_fb(terminal_bg);
}

void terminal_scroll(void) {
	fb_scroll(FONT_HEIGHT);
	/* Clear the last row */
	fb_fill_rect(0, (TERM_ROWS - 1) * FONT_HEIGHT,
	             TERM_COLS * FONT_WIDTH, FONT_HEIGHT, fb_bg);
}

void change_colour_current(enum vga_colour fg, enum vga_colour bg) {
	terminal_fg = fg;
	terminal_bg = bg;
	update_fb_colours();
}

void change_colour(enum vga_colour fg, enum vga_colour bg) {
	terminal_fg = fg;
	terminal_bg = bg;
	update_fb_colours();
	fb_clear(fb_bg);
	fb_flush();
	cursor_drawn = false;
}

static void erase_cursor(void) {
	if (!cursor_drawn) return;
	uint32_t x = (uint32_t)cursor_draw_col * FONT_WIDTH;
	uint32_t y = (uint32_t)cursor_draw_row * FONT_HEIGHT + FONT_HEIGHT - CURSOR_HEIGHT;
	fb_fill_rect(x, y, FONT_WIDTH, CURSOR_HEIGHT, cursor_draw_bg);
	cursor_drawn = false;
}

static void draw_cursor(void) {
	if (!cursor_visible) return;
	cursor_draw_row = terminal_row;
	cursor_draw_col = terminal_column;
	cursor_draw_bg = fb_bg;
	uint32_t x = (uint32_t)terminal_column * FONT_WIDTH;
	uint32_t y = (uint32_t)terminal_row * FONT_HEIGHT + FONT_HEIGHT - CURSOR_HEIGHT;
	fb_fill_rect(x, y, FONT_WIDTH, CURSOR_HEIGHT, fb_fg);
	cursor_drawn = true;
}

void enable_cursor(uint8_t cursor_start, uint8_t cursor_end) {
	(void)cursor_start;
	(void)cursor_end;
	cursor_visible = true;
	draw_cursor();
}

void disable_cursor() {
	erase_cursor();
	cursor_visible = false;
}

void update_cursor(int x, int y) {
	(void)x;
	(void)y;
}

void terminal_initialize(void) {
	framebuffer_info_t *info = fb_get_info();
	TERM_COLS = info->width  / FONT_WIDTH;
	TERM_ROWS = info->height / FONT_HEIGHT;
	terminal_row = 0;
	terminal_column = 0;
	cursor_drawn = false;
	update_fb_colours();
	fb_clear(fb_bg);
	fb_flush();
}

void terminal_setcolour(uint8_t colour) {
	terminal_fg = colour & 0x0F;
	terminal_bg = (colour >> 4) & 0x0F;
	update_fb_colours();
}

void terminal_putentryat(unsigned char c, uint8_t colour, size_t x, size_t y) {
	uint32_t fg = palette_to_fb(colour & 0x0F);
	uint32_t bg = palette_to_fb((colour >> 4) & 0x0F);
	fb_draw_char(c, x * FONT_WIDTH, y * FONT_HEIGHT, fg, bg);
}

void terminal_putchar(char c) {
	unsigned char uc = c;
	if (c == '\n') {
		deferred_wrap = false;
		if (terminal_row + 1 == TERM_ROWS) {
			terminal_scroll();
			terminal_column = 0;
		} else {
			terminal_row++;
			terminal_column = 0;
		}
	} else {
		/* If the previous character set deferred_wrap, apply the
		   wrap now before drawing the new character. */
		if (deferred_wrap) {
			deferred_wrap = false;
			terminal_column = 0;
			if (terminal_row + 1 == TERM_ROWS) {
				terminal_scroll();
			} else {
				terminal_row++;
			}
		}

		fb_draw_char(uc,
		             terminal_column * FONT_WIDTH,
		             terminal_row * FONT_HEIGHT,
		             fb_fg, fb_bg);

		if (terminal_column + 1 >= TERM_COLS) {
			/* Last column: defer the wrap */
			deferred_wrap = true;
		} else {
			terminal_column++;
		}
	}
}

/* ---- ANSI escape sequence handling ---- */

/* Map ANSI SGR color index (0-7) + bold bit to VGA palette */
static enum vga_colour ansi_to_vga_fg(int color, bool bold) {
	/* Standard ANSI order: black,red,green,yellow,blue,magenta,cyan,white */
	static const enum vga_colour normal[8] = {
		VGA_COLOUR_BLACK, VGA_COLOUR_RED, VGA_COLOUR_GREEN, VGA_COLOUR_BROWN,
		VGA_COLOUR_BLUE, VGA_COLOUR_MAGENTA, VGA_COLOUR_CYAN, VGA_COLOUR_LIGHT_GREY
	};
	static const enum vga_colour bright[8] = {
		VGA_COLOUR_DARK_GREY, VGA_COLOUR_LIGHT_RED, VGA_COLOUR_LIGHT_GREEN, VGA_COLOUR_LIGHT_BROWN,
		VGA_COLOUR_LIGHT_BLUE, VGA_COLOUR_LIGHT_MAGENTA, VGA_COLOUR_LIGHT_CYAN, VGA_COLOUR_WHITE
	};
	return bold ? bright[color & 7] : normal[color & 7];
}

static void ansi_finalize_param(void) {
	if (ansi_param_count < ANSI_MAX_PARAMS) {
		ansi_params[ansi_param_count++] = ansi_has_digit ? ansi_cur_param : -1;
	}
	ansi_cur_param = 0;
	ansi_has_digit = false;
}

static void ansi_reset(void) {
	ansi_state = ANSI_NORMAL;
	ansi_param_count = 0;
	ansi_cur_param = 0;
	ansi_has_digit = false;
}

static int ansi_param(int idx, int def) {
	if (idx >= ansi_param_count || ansi_params[idx] <= 0)
		return def;
	return ansi_params[idx];
}

static void ansi_clear_line_part(int mode) {
	uint32_t y = (uint32_t)terminal_row * FONT_HEIGHT;
	switch (mode) {
	case 0: /* cursor to end */
		fb_fill_rect((uint32_t)terminal_column * FONT_WIDTH, y,
		             (uint32_t)(TERM_COLS - terminal_column) * FONT_WIDTH, FONT_HEIGHT, fb_bg);
		break;
	case 1: /* start to cursor */
		fb_fill_rect(0, y, (uint32_t)(terminal_column + 1) * FONT_WIDTH, FONT_HEIGHT, fb_bg);
		break;
	case 2: /* entire line */
		fb_fill_rect(0, y, (uint32_t)TERM_COLS * FONT_WIDTH, FONT_HEIGHT, fb_bg);
		break;
	}
}

static void ansi_clear_screen_part(int mode) {
	switch (mode) {
	case 0: /* cursor to end */
		/* Clear rest of current line */
		ansi_clear_line_part(0);
		/* Clear lines below */
		if (terminal_row + 1 < TERM_ROWS) {
			fb_fill_rect(0, (uint32_t)(terminal_row + 1) * FONT_HEIGHT,
			             (uint32_t)TERM_COLS * FONT_WIDTH,
			             (uint32_t)(TERM_ROWS - terminal_row - 1) * FONT_HEIGHT, fb_bg);
		}
		break;
	case 1: /* start to cursor */
		if (terminal_row > 0) {
			fb_fill_rect(0, 0, (uint32_t)TERM_COLS * FONT_WIDTH,
			             (uint32_t)terminal_row * FONT_HEIGHT, fb_bg);
		}
		ansi_clear_line_part(1);
		break;
	case 2: /* entire screen */
		fb_clear(fb_bg);
		break;
	}
}

static void ansi_scroll_up(int n) {
	if (n <= 0) n = 1;
	if ((size_t)n >= TERM_ROWS) {
		fb_clear(fb_bg);
		return;
	}
	fb_scroll((uint32_t)n * FONT_HEIGHT);
	fb_fill_rect(0, (uint32_t)(TERM_ROWS - (size_t)n) * FONT_HEIGHT,
	             (uint32_t)TERM_COLS * FONT_WIDTH, (uint32_t)n * FONT_HEIGHT, fb_bg);
}

static void ansi_scroll_down(int n) {
	if (n <= 0) n = 1;
	if ((size_t)n >= TERM_ROWS) {
		fb_clear(fb_bg);
		return;
	}
	uint32_t copy_h = (uint32_t)(TERM_ROWS - (size_t)n) * FONT_HEIGHT;
	/* Move screen content down */
	memmove(fb_get_info()->address + (uint32_t)n * FONT_HEIGHT * fb_get_info()->pitch,
	        fb_get_info()->address,
	        copy_h * fb_get_info()->pitch);
	fb_fill_rect(0, 0, (uint32_t)TERM_COLS * FONT_WIDTH, (uint32_t)n * FONT_HEIGHT, fb_bg);
}

static void ansi_handle_sgr(void) {
	static bool bold = false;
	if (ansi_param_count == 0) {
		/* ESC[m = reset */
		terminal_fg = VGA_COLOUR_LIGHT_GREY;
		terminal_bg = VGA_COLOUR_BLACK;
		bold = false;
		update_fb_colours();
		return;
	}
	for (int i = 0; i < ansi_param_count; i++) {
		int p = ansi_param(i, 0);
		if (p == 0) {
			terminal_fg = VGA_COLOUR_LIGHT_GREY;
			terminal_bg = VGA_COLOUR_BLACK;
			bold = false;
		} else if (p == 1) {
			bold = true;
		} else if (p == 7) {
			/* Reverse video */
			uint8_t tmp = terminal_fg;
			terminal_fg = terminal_bg;
			terminal_bg = tmp;
		} else if (p == 27) {
			/* Un-reverse: just reset to defaults */
			terminal_fg = VGA_COLOUR_LIGHT_GREY;
			terminal_bg = VGA_COLOUR_BLACK;
		} else if (p == 22) {
			bold = false;
		} else if (p >= 30 && p <= 37) {
			terminal_fg = ansi_to_vga_fg(p - 30, bold);
		} else if (p == 39) {
			terminal_fg = bold ? VGA_COLOUR_WHITE : VGA_COLOUR_LIGHT_GREY;
		} else if (p >= 40 && p <= 47) {
			terminal_bg = ansi_to_vga_fg(p - 40, false);
		} else if (p == 49) {
			terminal_bg = VGA_COLOUR_BLACK;
		} else if (p >= 90 && p <= 97) {
			/* Bright foreground */
			terminal_fg = ansi_to_vga_fg(p - 90, true);
		} else if (p >= 100 && p <= 107) {
			/* Bright background */
			terminal_bg = ansi_to_vga_fg(p - 100, true);
		}
	}
	update_fb_colours();
}

static void ansi_execute_csi(char final) {
	int n;
	switch (final) {
	case 'A': /* CUU – Cursor Up */
		n = ansi_param(0, 1);
		if ((size_t)n > terminal_row) terminal_row = 0;
		else terminal_row -= (size_t)n;
		break;
	case 'B': /* CUD – Cursor Down */
		n = ansi_param(0, 1);
		terminal_row += (size_t)n;
		if (terminal_row >= TERM_ROWS) terminal_row = TERM_ROWS - 1;
		break;
	case 'C': /* CUF – Cursor Forward */
		n = ansi_param(0, 1);
		terminal_column += (size_t)n;
		if (terminal_column >= TERM_COLS) terminal_column = TERM_COLS - 1;
		break;
	case 'D': /* CUB – Cursor Back */
		n = ansi_param(0, 1);
		if ((size_t)n > terminal_column) terminal_column = 0;
		else terminal_column -= (size_t)n;
		break;
	case 'H': /* CUP – Cursor Position (1-based) */
	case 'f':
		deferred_wrap = false;
		terminal_row = (size_t)(ansi_param(0, 1) - 1);
		terminal_column = (size_t)(ansi_param(1, 1) - 1);
		if (terminal_row >= TERM_ROWS) terminal_row = TERM_ROWS - 1;
		if (terminal_column >= TERM_COLS) terminal_column = TERM_COLS - 1;
		break;
	case 'J': /* ED – Erase in Display */
		ansi_clear_screen_part(ansi_param(0, 0));
		break;
	case 'K': /* EL – Erase in Line */
		ansi_clear_line_part(ansi_param(0, 0));
		break;
	case 'm': /* SGR – Select Graphic Rendition */
		ansi_handle_sgr();
		break;
	case 's': /* SCP – Save Cursor Position */
		saved_row = terminal_row;
		saved_col = terminal_column;
		break;
	case 'u': /* RCP – Restore Cursor Position */
		terminal_row = saved_row;
		terminal_column = saved_col;
		break;
	case 'S': /* SU – Scroll Up */
		ansi_scroll_up(ansi_param(0, 1));
		break;
	case 'T': /* SD – Scroll Down */
		ansi_scroll_down(ansi_param(0, 1));
		break;
	}
}

static void ansi_execute_csi_qmark(char final) {
	int p = ansi_param(0, 0);
	if (p == 25) {
		if (final == 'h') cursor_visible = true;   /* Show cursor */
		else if (final == 'l') cursor_visible = false; /* Hide cursor */
	}
	/* Other DEC private modes silently ignored */
}

static void terminal_write_char(char c) {
	switch (ansi_state) {
	case ANSI_NORMAL:
		if (c == '\x1b') {
			ansi_state = ANSI_ESC;
			ansi_param_count = 0;
			ansi_cur_param = 0;
			ansi_has_digit = false;
		} else {
			terminal_putchar(c);
		}
		break;

	case ANSI_ESC:
		if (c == '[') {
			ansi_state = ANSI_CSI;
		} else {
			/* Not a CSI sequence – emit ESC and the char */
			ansi_reset();
			terminal_putchar(c);
		}
		break;

	case ANSI_CSI:
		if (c == '?') {
			ansi_state = ANSI_CSI_QMARK;
		} else if (c >= '0' && c <= '9') {
			ansi_cur_param = ansi_cur_param * 10 + (c - '0');
			ansi_has_digit = true;
		} else if (c == ';') {
			ansi_finalize_param();
		} else if (c >= 0x40 && c <= 0x7E) {
			/* Final byte */
			ansi_finalize_param();
			ansi_execute_csi(c);
			ansi_reset();
		} else {
			/* Unknown intermediate – abort */
			ansi_reset();
		}
		break;

	case ANSI_CSI_QMARK:
		if (c >= '0' && c <= '9') {
			ansi_cur_param = ansi_cur_param * 10 + (c - '0');
			ansi_has_digit = true;
		} else if (c == ';') {
			ansi_finalize_param();
		} else if (c >= 0x40 && c <= 0x7E) {
			ansi_finalize_param();
			ansi_execute_csi_qmark(c);
			ansi_reset();
		} else {
			ansi_reset();
		}
		break;
	}
}

void terminal_write(const char* data, size_t size) {
	/* Mirror to serial port for debug capture */
	serial_write(data, size);

	erase_cursor();
	for (size_t i = 0; i < size && data[i] != '\0'; i++) {
		if (data[i] == '\r') {
			terminal_column = 0;
			deferred_wrap = false;
			continue;
		} else if (data[i] == '\b') {
			if (terminal_column > 0) {
				terminal_column--;
				fb_draw_char(' ',
				             terminal_column * FONT_WIDTH,
				             terminal_row * FONT_HEIGHT,
				             fb_fg, fb_bg);
			}
			continue;
		} else if (data[i] == 0x09) {
			for (int t = 0; t < 5; t++)
				terminal_putchar(' ');
			continue;
		}
		terminal_write_char(data[i]);
	}
	draw_cursor();
	fb_flush();
}

void terminal_writestring(const char* data) {
	terminal_write(data, strlen(data));
}

size_t terminal_get_rows(void) { return TERM_ROWS; }
size_t terminal_get_cols(void) { return TERM_COLS; }
