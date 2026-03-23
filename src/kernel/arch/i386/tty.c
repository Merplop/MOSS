#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/io.h>
#include <kernel/tty.h>

#include "framebuffer.h"
#include "font8x16.h"

/* Terminal dimensions in characters (computed from framebuffer size) */
static size_t TERM_COLS;
static size_t TERM_ROWS;

static size_t terminal_row;
static size_t terminal_column;
static uint8_t terminal_fg = VGA_COLOUR_LIGHT_GREY;
static uint8_t terminal_bg = VGA_COLOUR_BLACK;
static uint32_t fb_fg;
static uint32_t fb_bg;

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
}

void enable_cursor(uint8_t cursor_start, uint8_t cursor_end) {
	(void)cursor_start;
	(void)cursor_end;
}

void disable_cursor() {
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
	update_fb_colours();
	fb_clear(fb_bg);
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
		if (terminal_row + 1 == TERM_ROWS) {
			terminal_scroll();
			terminal_column = 0;
		} else {
			terminal_row++;
			terminal_column = 0;
		}
	} else {
		fb_draw_char(uc,
		             terminal_column * FONT_WIDTH,
		             terminal_row * FONT_HEIGHT,
		             fb_fg, fb_bg);
		if (++terminal_column == TERM_COLS) {
			terminal_column = 0;
			if (++terminal_row == TERM_ROWS)
				terminal_scroll();
		}
	}
}

void terminal_write(const char* data, size_t size) {
	for (size_t i = 0; i < size && data[i] != '\0'; i++) {
		if (data[i] == '\r') {
			terminal_column = 0;
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
		terminal_putchar(data[i]);
	}
}

void terminal_writestring(const char* data) {
	terminal_write(data, strlen(data));
}
