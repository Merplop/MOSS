/* 
 * mpad - vi-like screen-oriented text editor
 * MOSS port
 * Miro Haapalainen
 */

#include <termios.h>
#include <unistd.h>
#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include <sys/ioctl.h>
#include <string.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <ctype.h>
#include <moss/keyboard.h>

#define MIN(a,b) ((a) < (b) ? (a) : (b))
#define MAX(a,b) ((a) > (b) ? (a) : (b))

#define DEFAULT_BUF_CAP 128 	// Default buffer line count
#define DEFAULT_LINE_CAP 16 	// Default (empty) line cap

#define ESC 27
#define ENTER 13
#define BACKSPACE 8
#define DEL 127
#define TAB 9

#define TAB_WIDTH 4
typedef struct {
    size_t row;
    size_t col;
} Cursor;


// The current window/viewport of
// the text editor; i.e. what is the topmost
// visible row?
typedef struct {
    size_t top_line;
    size_t top_rowoff; // Wrapped-row offset inside top_line
} CurrentView;

enum EditorKey {
    ARROW_LEFT = 1000,
    ARROW_RIGHT,
    ARROW_UP,
    ARROW_DOWN
};

enum Mode {
    NORMAL,
    INSERT,
    COMMAND
};

typedef struct {
    char *data;
    size_t len;
    size_t cap;

	unsigned char *hl;
	size_t hl_cap;
	bool hl_open_comment;
} Line;

typedef struct {
    Line *lines;
    size_t line_count;
    size_t cap;
} Buffer;

/* For syntax highlighting */
enum Highlight {
	HL_NORMAL = 0,
	HL_COMMENT,
	HL_MLCOMMENT,
	HL_STRING,
	HL_NUMBER,
	HL_KEYWORD
};

static const char *C_KEYWORDS[] = {
    "if","else","for","while","do","switch","case","default","break","continue","return",
    "struct","typedef","enum","union",
    "static","const","volatile","extern","inline",
    "void","char","short","int","long","float","double","signed","unsigned","size_t","bool",
    "NULL",
    NULL
};


/* -------- globals --------- */

Buffer global_buffer;
Cursor global_cursor;
CurrentView global_view = {0, 0}; 
struct termios orig_termios;

static const char *global_filename = NULL;
static char *global_filename_owned = NULL;

static enum Mode global_mode = NORMAL;

static char global_control_char = ' ';

// Dirty bit - buffer has been modified
// but not yet synced with file
static bool global_dirty = false;

static char global_status[128] = "";
static char global_cmd[64] = "";
static size_t global_cmd_len = 0;

static bool line_num = true;

static bool editor_running = true;


/* -------- misc helpers ------- */

static void die(const char *msg) {
    write(STDOUT_FILENO, "\x1b[?25h\x1b[2J\x1b[H", 13);
    perror(msg);
    exit(1);
}

int get_window_size(int *rows, int *cols) {
    struct winsize ws;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == -1) {
        return -1;
    }

    *rows = ws.ws_row;
    *cols = ws.ws_col;
    return 0;
}

/* ------ dynamic append buffer ------- */

struct abuf {
    char *b;
    int len;
    int cap;
};
#define ABUF_INIT {NULL, 0, 0}
#define ABUF_INITIAL_CAP 32768  /* 32KB — enough for a full screen refresh */

static void abAppend(struct abuf *ab, const char *s, int len) {
    if (ab->len + len > ab->cap) {
        int new_cap = ab->cap ? ab->cap : ABUF_INITIAL_CAP;
        while (new_cap < ab->len + len)
            new_cap *= 2;
        char *newbuf = realloc(ab->b, (size_t)new_cap);
        if (!newbuf) die("realloc");
        ab->b = newbuf;
        ab->cap = new_cap;
    }
    memcpy(&ab->b[ab->len], s, (size_t)len);
    ab->len += len;
}

static void abFree(struct abuf *ab) {
    free(ab->b);
    ab->b = NULL;
    ab->len = 0;
    ab->cap = 0;
}

/* ----- buffer/line primitives ------- */

static void line_reserve(Line *l, size_t needed) {
    if (needed <= l->cap) return;
    if (l->cap == 0) l->cap = DEFAULT_LINE_CAP;
    while (l->cap < needed) {
        l->cap *= 2;
    }
    char *p = realloc(l->data, l->cap);
    if (!p) die("realloc");
    l->data = p;
}

static void line_insert_char(Line *l, size_t pos, char c) {
    if (pos > l->len) {
        pos = l->len;
    }
    line_reserve(l, l->len+2);
    memmove(&l->data[pos+1], &l->data[pos], l->len - pos + 1);
    l->data[pos] = c;
    l->len++;
}

static void line_delete_char(Line *l, size_t pos) {
    if (pos >= l->len) return;

    memmove(&l->data[pos], &l->data[pos+1], l->len - pos);

    l->len--;
}

static void line_append_bytes(Line *l, const char *s, size_t n) {
    if (!n) return;
    line_reserve(l, l->len+n+1);
    memcpy(&l->data[l->len], s, n);
    l->len += n;
    l->data[l->len] = '\0';
}

static void buffer_free(Buffer *b) {
    if (!b->lines) return;
    for (size_t i = 0; i < b->line_count; i++) {
        free(b->lines[i].data);
        free(b->lines[i].hl); 
    }
    free(b->lines);
    b->lines = NULL;
    b->line_count = 0;
    b->cap = 0;
}

static void buffer_append_line(Buffer *b, char **text, size_t len) {
    if (b->line_count + 1 > b->cap) {
        b->cap *= 2;
	b->lines = realloc(b->lines, b->cap * sizeof(Line));
    }

    Line *l = &b->lines[b->line_count];

    l->cap = len + 1;
    l->data = malloc(l->cap);
    memcpy(l->data, *text, len);
    l->data[len] = '\0';
    l->len = len;

    b->line_count++;
}

static void buffer_init(Buffer *b) {
    b->cap = DEFAULT_BUF_CAP;
    b->line_count = 1;
    b->lines = calloc(b->cap, sizeof(Line));
    if (!b->lines) die("calloc");

    b->lines[0].cap = DEFAULT_LINE_CAP; // Start with one empty line
    b->lines[0].data = calloc(b->lines[0].cap, 1);   // Zero out memory space for line
    if (!b->lines[0].data) die("calloc");
    b->lines[0].len = 0;
}

static void buffer_ensure_line_capacity(Buffer *b, size_t needed_lines) {
    if (needed_lines <= b->cap) return;
    while (b->cap < needed_lines) b->cap *= 2;
    Line *p = realloc(b->lines, b->cap * sizeof(Line));
    if (!p) die("realloc");
    memset(p + b->line_count, 0, (b->cap - b->line_count) * sizeof(Line));
    b->lines = p;
}

static void buffer_insert_line(Buffer *b, size_t row) {
    if (row > b->line_count) {
        row = b->line_count;
    }

    buffer_ensure_line_capacity(b, b->line_count + 1);

    memmove(&b->lines[row + 1], &b->lines[row], (b->line_count - row) * sizeof(Line));

    Line *l = &b->lines[row];
    memset(l, 0, sizeof(*l));
    l->cap = DEFAULT_LINE_CAP;
    l->data = calloc(l->cap, 1);
    if (!l->data) die("calloc");
    l->len = 0;

    b->line_count++;
}

static void buffer_delete_line(Buffer *b, size_t row) {
    if (b->line_count == 0 || row >= b->line_count) return;

    if (b->line_count == 1) {
        b->lines[0].len = 0;
        b->lines[0].data[0] = '\0';
        return;
    }

    free(b->lines[row].data);
    free(b->lines[row].hl);
    memmove(&b->lines[row], &b->lines[row + 1], (b->line_count - row - 1) * sizeof(Line));
    b->line_count--;
}

static void buffer_split_line(Buffer *b, Cursor *c) {
    if (c->row >= b->line_count) return;

    Line *cur = &b->lines[c->row];
    c->col = MIN(c->col, cur->len);

    buffer_insert_line(b, c->row + 1);
    Line *next = &b->lines[c->row + 1];

    size_t tail_len = cur->len - c->col;
    line_reserve(next, tail_len + 1);

    memcpy(next->data, &cur->data[c->col], tail_len);
    next->data[tail_len] = '\0';
    next->len = tail_len;

    cur->len = c->col;
    cur->data[cur->len] = '\0';

    c->row++;
    c->col = 0;
}

static void buffer_join_line_with_prev(Buffer *b, Cursor *c) {
    if (c->row == 0 || c->row >= b->line_count) return;

    Line *prev = &b->lines[c->row - 1];
    Line *cur = &b->lines[c->row];

    size_t prev_len = prev->len;
    line_append_bytes(prev, cur->data, cur->len);

    buffer_delete_line(b, c->row);
    c->row--;
    c->col = prev_len;
}

static void buffer_append_line_owned(Buffer *b, const char *text, size_t len) {
    buffer_ensure_line_capacity(b, b->line_count+1);

    Line *l = &b->lines[b->line_count];
    memset(l, 0, sizeof(*l));

    l->cap = MAX(DEFAULT_LINE_CAP, len+1);
    l->data = malloc(l->cap);
    if (!l->data) die("malloc");
    if (len) memcpy(l->data, text, len);
    l->data[len] = '\0';
    l->len = len;

    b->line_count++;
}

int buffer_load_file(Buffer *b, FILE *fp) {
    buffer_free(b);

    b->cap = DEFAULT_BUF_CAP;
    b->line_count = 0;
    b->lines = calloc(b->cap, sizeof(Line));
    if (!b->lines) die("calloc");

    char *line = NULL;
    size_t cap = 0;
    ssize_t n;

    while ((n = getline(&line, &cap, fp)) != -1) {
        if (n > 0 && line[n-1] == '\n') {
	        n--;
	    }

	    buffer_append_line_owned(b, line, (size_t)n);
    }
    free(line);
    fclose(fp);

    if (b->line_count == 0) {
        buffer_append_line_owned(b, "", 0);
    }

    return 0;
}

int dump_buffer_to_file(Buffer *b, const char *path) {

    if (!path || !*path) {
        snprintf(global_status, sizeof(global_status), "No file name (use :w <path>)");
        return 1;
    }

    FILE *fp = fopen(path, "w");	

    if (!fp) {
        snprintf(global_status, sizeof(global_status), "Write failed: %s", strerror(errno));
        return 1;
    }
	
    for (size_t i = 0; i < b->line_count; i++) {
        if (b->lines[i].len > 0) {

            size_t wrote = fwrite(b->lines[i].data, 1, b->lines[i].len, fp);

            if (wrote != b->lines[i].len) {
                snprintf(global_status, sizeof(global_status), "Write failed: %s", strerror(errno));
                fclose(fp);
                return 1;
            }
        }
        
        if (i + 1 < b->line_count) {
            fputc('\n', fp);
        }
    }

    fclose(fp);
    global_dirty = false;
    snprintf(global_status, sizeof(global_status), "Wrote %s", path);
    return 0;
}

/* ----- raw mode funcs ------ */

void disable_raw_mode() {
    tcsetattr(STDIN_FILENO, TCSAFLUSH, &orig_termios);
}

void enable_raw_mode() {
    tcgetattr(STDIN_FILENO, &orig_termios);
    atexit(disable_raw_mode);
    /* On MOSS the keyboard is always raw — nothing else to do */
}

/* ----- key reading ------ */

/*
 * MOSS keyboard delivers raw scancodes + ASCII via key_event_t.
 * Arrow keys are extended scancodes (0xE0 prefix):
 *   Up=0x48, Down=0x50, Left=0x4B, Right=0x4D
 * We translate these into the editor's internal ARROW_* codes.
 */
static int editor_read_key(void) {
    key_event_t ev;

    while (1) {
        moss_wait_key(&ev);

        /* Only handle key presses */
        if (!(ev.flags & KEY_EVENT_PRESS))
            continue;

        /* Extended keys: arrow keys */
        if (ev.flags & KEY_EVENT_EXTENDED) {
            switch (ev.scancode) {
                case 0x48: return ARROW_UP;
                case 0x50: return ARROW_DOWN;
                case 0x4B: return ARROW_LEFT;
                case 0x4D: return ARROW_RIGHT;
            }
            continue; /* ignore other extended keys */
        }

        /* ESC key */
        if (ev.scancode == 0x01)
            return ESC;

        /* If there's an ASCII translation, return it */
        if (ev.ascii != 0)
            return (unsigned char)ev.ascii;
    }
}

/* ------ syntax highlighting ------ */

static int hl_to_ansi(enum Highlight hl) {
    switch (hl) {
		case HL_COMMENT:      return 90; // gray
		case HL_MLCOMMENT:    return 90; // gray
		case HL_STRING:       return 32; // green
		case HL_NUMBER:       return 36; // cyan
		case HL_KEYWORD:      return 33; // yellow
		default: 			  return 39; // default fg		
	}
}

static bool is_separator(int c) {
    unsigned char uc = (unsigned char)c;
    return isspace(uc) || c == '\0' ||
           strchr(",.()+-/*=~%<>[]{};:&|^!?", uc) != NULL;
}
static bool filename_is_c_like(const char *fn) {
    if (!fn) return false;
    const char *dot = strrchr(fn, '.');
    if (!dot) return false;
    return strcmp(dot, ".c") == 0 || strcmp(dot, ".h") == 0 || strcmp(dot, ".cpp") == 0 ||
        strcmp(dot, ".hpp") == 0;
}

// Computes l->hl[i] and whether the line ends inside multiline comment
static void line_hl_reserve(Line *l, size_t needed) {
    if (needed <= l->hl_cap) return;
    size_t cap = (l->hl_cap == 0) ? 16 : l->hl_cap;
    while (cap < needed) cap *= 2;
    unsigned char *p = realloc(l->hl, cap);
    if (!p) die("realloc");
    l->hl = p;
    l->hl_cap = cap;
}

static bool is_keyword(const char *s, size_t len) {
    for (int k = 0; C_KEYWORDS[k]; k++) {
        if (strlen(C_KEYWORDS[k]) == len && memcmp(C_KEYWORDS[k], s, len) == 0) return true;
    }
    return false;
}

static bool editor_update_syntax_line(size_t row, bool in_comment) {
    Line *l = &global_buffer.lines[row];
    line_hl_reserve(l, l->len);
    memset(l->hl, HL_NORMAL, l->len);

    size_t i = 0;
    while (i < l->len) {
        char c = l->data[i];

        if (in_comment) {
            l->hl[i] = HL_MLCOMMENT;
            if (c == '*' && i + 1 < l->len && l->data[i+1] == '/') {
                l->hl[i] = HL_MLCOMMENT;
                l->hl[i+1] = HL_MLCOMMENT;
                i += 2;
                in_comment = false;
                continue;
            }
            i++;
            continue;
        }

        if (c == '/' && i + 1 < l->len && l->data[i+1] == '/') {
            for (size_t j = i; j < l->len; j++) l->hl[j] = HL_COMMENT;
            break;
        }

        if (c == '/' && i + 1 < l->len && l->data[i + 1] == '*') {
            l->hl[i] = HL_MLCOMMENT;
            l->hl[i+1] = HL_MLCOMMENT;
            i += 2;
            in_comment = true;
            continue;
        }

        if (c == '"' || c == '\'') {
            char quote = c;
            l->hl[i++] = HL_STRING;
            while (i < l->len) {
                l->hl[i] = HL_STRING;
                if (l->data[i] == '\\' && i+1 < l->len) {
                    l->hl[i+1] = HL_STRING;
                    i += 2;
                    continue;
                }
                if (l->data[i] == quote) {i++; break;}
                i++;
            }
            continue;
        }

        if (isdigit((unsigned char)c) && (i == 0 || is_separator(l->data[i-1]))) {
            size_t j = i;
            while (j < l->len && (isdigit((unsigned char)l->data[j]) || l->data[j]=='.')) {
                l->hl[j] = HL_NUMBER;
                j++;
            }
            i = j;
            continue;
        }

        if (isalpha((unsigned char)c) || c == '_') {
            size_t start = i;
            size_t j = i;
            while (j < l->len && (isalnum((unsigned char)l->data[j]) || l->data[j]=='_')) j++; 
            bool kw = is_keyword(&l->data[start], j - start);
            if (kw && (start == 0 || is_separator(l->data[start - 1])) &&
                (j == l->len || is_separator(l->data[j]))) {
                    for (size_t k = start; k < j; k++) l->hl[k] = HL_KEYWORD;
                }

                i = j;
                continue;
        }
        i++;
    }
    bool changed = (l->hl_open_comment != in_comment);
    l->hl_open_comment = in_comment;
    return changed;
}

static void editor_update_syntax_from(size_t start_row) {
    if (!filename_is_c_like(global_filename)) return;
    if (start_row >= global_buffer.line_count) return;

    bool in_comment = false;
    if (start_row > 0) in_comment = global_buffer.lines[start_row - 1].hl_open_comment;

    for (size_t r = start_row; r < global_buffer.line_count; r++) {
        bool prev_open = global_buffer.lines[r].hl_open_comment;

        editor_update_syntax_line(r, in_comment);
        in_comment = global_buffer.lines[r].hl_open_comment;

        bool next_missing = (r + 1 < global_buffer.line_count) &&
                            (global_buffer.lines[r + 1].hl == NULL);

        if (global_buffer.lines[r].hl_open_comment == prev_open && !next_missing) {
            break;
        }
    }
}

/* ------ screen mapping/render helpers ------- */

// How many terminal columns does this prefix of the
// line occupy?
static int visual_width_upto(const Line *l, size_t upto_col) {
    int width = 0;
    size_t end = MIN(upto_col, l->len);

    for (size_t i = 0; i < end; i++) {
        if (l->data[i] == '\t') {
            int add = TAB_WIDTH - (width % TAB_WIDTH);
	        width += add;
	    } else {
	        width += 1;
	    }
    }
    return width;
}

// How many screen rows does a line occupy?
static int screen_rows_for_line(const Line *l, int screen_cols) {
    int vwidth = visual_width_upto(l, l->len);
    return MAX(1, (vwidth + screen_cols - 1) / screen_cols); 
}


void buffer_to_screen_unclipped(
        const Buffer *b,
        size_t target_line,
        size_t target_col,
        const CurrentView *view,
        int screen_cols,
        int *out_row, int *out_col) {
    
    int row = -(int)view->top_rowoff;

    if (target_line < view->top_line) {
        *out_row = -999999;
        *out_col = 0;
        return;
    }

    for (size_t l = view->top_line; l < target_line && l < b->line_count; l++) {
        row += screen_rows_for_line(&b->lines[l], screen_cols);
    }

    if (target_line >= b->line_count) {
        *out_row = 999999;
        *out_col = 0;
        return;
    }

    const Line *cur = &b->lines[target_line];
    target_col = MIN(target_col, cur->len);

    int vcol = visual_width_upto(cur, target_col);
    row += (screen_cols > 0) ? (vcol / screen_cols) : 0;
    int col = (screen_cols > 0) ? (vcol % screen_cols) : 0;

    *out_row = row;
    *out_col = col;
}

static bool buffer_to_screen(
        const Buffer *b,
        size_t target_line,
        size_t target_col,
        const CurrentView *view,
        int screen_cols,
        int text_rows,
        int *out_row, int *out_col) {

    int r, c;
    buffer_to_screen_unclipped(b, target_line, target_col, view, screen_cols, &r, &c);
    if (r < 0 || r >= text_rows) return false;
    *out_row = r;
    *out_col = c;
    return true;
}

static void view_scroll_by_rows(CurrentView *view, const Buffer *b, int screen_cols, int delta_rows) {
    if (b->line_count == 0) {
        view->top_line = 0;
        view->top_rowoff = 0;
        return;
    }

    if (delta_rows > 0) {
        for (int i = 0; i < delta_rows; i++) {
            int rows_in_line = screen_rows_for_line(&b->lines[view->top_line], screen_cols);
            if (view->top_rowoff + 1 < (size_t)rows_in_line) {
                view->top_rowoff++;
            } else {
                if (view->top_line + 1 >= b->line_count) break;
                view->top_line++;
                view->top_rowoff = 0;
            }
        }
    } else if (delta_rows < 0) {
        for (int i = 0; i < -delta_rows; i++) {
            if (view->top_rowoff > 0) {
                view->top_rowoff--;
            } else {
                if (view->top_line == 0) break;
                view->top_line--;
                int rows_in_prev = screen_rows_for_line(&b->lines[view->top_line], screen_cols);
                view->top_rowoff = (rows_in_prev > 0) ? (size_t)(rows_in_prev - 1) : 0;
            }
        }
    }
}

static void editor_scroll_to_cursor(void) {
    int rows, cols;
    if (get_window_size(&rows, &cols) == -1) return;
    int text_rows = rows - 1;

    int cr, cc;
    buffer_to_screen_unclipped(&global_buffer, global_cursor.row, global_cursor.col, &global_view, cols, &cr, &cc);
    int delta = 0;
    if (cr < 0) delta = cr;
    else if (cr >= text_rows) delta = cr - (text_rows - 1);

    if (delta != 0) view_scroll_by_rows(&global_view, &global_buffer, cols, delta);
}

/* ----- rendering ----- */

static void editor_append_wrapped_slice_hl(struct abuf *ab, const Line *l, int text_cols, size_t wrap_row) {
    int start_v = (int)(wrap_row * (size_t)text_cols);
    int end_v   = start_v + text_cols;

    int v = 0;
    int cur_color = 39;

    for (size_t i = 0; i < l->len; i++) {
        unsigned char hl = (l->hl && i < l->len) ? l->hl[i] : HL_NORMAL;
        int color = hl_to_ansi((enum Highlight)hl);

        if (l->data[i] == '\t') {
            int spaces = TAB_WIDTH - (v % TAB_WIDTH);
            for (int s = 0; s < spaces; s++) {
                if (v >= start_v && v < end_v) {
                    if (color != cur_color) {
                        char buf[16];
                        int n = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                        abAppend(ab, buf, n);
                        cur_color = color;
                    }
                    abAppend(ab, " ", 1);
                }
                v++;
                if (v >= end_v) goto done;
            }
        } else {
            if (v >= start_v && v < end_v) {
                if (color != cur_color) {
                    char buf[16];
                    int n = snprintf(buf, sizeof(buf), "\x1b[%dm", color);
                    abAppend(ab, buf, n);
                    cur_color = color;
                }
                abAppend(ab, &l->data[i], 1);
            }
            v++;
            if (v >= end_v) break;
        }
    }

done:
    if (cur_color != 39) abAppend(ab, "\x1b[39m", 5); // reset to default fg
}

static void editor_append_wrapped_slice(struct abuf *ab, const Line *l, int screen_cols, size_t wrap_row) {
    int start_v = (int)(wrap_row * (size_t)screen_cols);
    int end_v = start_v + screen_cols;

    int v = 0;
    for (size_t i = 0; i < l->len; i++) {
        char ch = l->data[i];

        if (ch == '\t') {
            int spaces = TAB_WIDTH - (v % TAB_WIDTH);
            for (int s = 0; s < spaces; s++) {
                if (v >= start_v && v < end_v) abAppend(ab, " ", 1);
                v++;
                if (v >= end_v) return;
            }
        } else {
            if (v >= start_v && v < end_v) abAppend(ab, &ch, 1);
            v++;
            if (v >= end_v) return;
        }
    }
}

static void editor_draw_rows(struct abuf *ab, int text_rows, int screen_cols) {
    size_t line_idx = global_view.top_line;
    size_t rowoff = global_view.top_rowoff;

    for (int y = 0; y < text_rows; y++) {
        if (line_idx >= global_buffer.line_count) {
            abAppend(ab, "~", 1);
        } else {
            Line *l = &global_buffer.lines[line_idx];
        
            editor_append_wrapped_slice_hl(ab, l, screen_cols, rowoff);

            int rows_in_line = screen_rows_for_line(l, screen_cols);
            if (rowoff + 1 < (size_t)rows_in_line) {
                rowoff++;
            } else {
                line_idx++;
                rowoff = 0;
            }
        } 
        
        abAppend(ab, "\x1b[K", 3);
        abAppend(ab, "\r\n", 2);
    }
}

static void editor_draw_status_bar(struct abuf *ab, int screen_cols) {
    char left[256];
    left[0] = '\0';

    if (global_mode == COMMAND) {
        snprintf(left, sizeof(left), ":%s", global_cmd);
    } else if (global_status[0] != '\0') {
        snprintf(left, sizeof(left), "%s", global_status);
    } else {
        const char *mode = (global_mode == INSERT) ? "INSERT" : "NORMAL";
        const char *fname = global_filename ? global_filename : "[No Name]";
        snprintf(left, sizeof(left),
                "\"%s\"%s  %s  Ln %zu, Col %zu                    %c",
                fname,
                global_dirty ? " [+]" : "",
                mode,
                global_cursor.row + 1,
                global_cursor.col + 1,
                global_control_char);
    }

    abAppend(ab, "\x1b[7m", 4);
    int len = (int)strlen(left);
    if (len > screen_cols) len = screen_cols;
    abAppend(ab, left, len);
    while (len < screen_cols) {
        abAppend(ab, " ", 1);
        len++;
    }
    abAppend(ab, "\x1b[m", 3);
}

static void editor_refresh_screen(void) {
    editor_scroll_to_cursor();

    int rows, cols;
    if (get_window_size(&rows, &cols) == -1) return;

    int text_rows = rows - 1;
    if (text_rows < 1) text_rows = 1;

    struct abuf ab = ABUF_INIT;

    abAppend(&ab, "\x1b[?25l", 6);
    abAppend(&ab, "\x1b[H", 3);

    editor_draw_rows(&ab, text_rows, cols);
    editor_draw_status_bar(&ab, cols);

    int r = 0, c = 0;
    if (!buffer_to_screen(&global_buffer, global_cursor.row, global_cursor.col, &global_view, cols, text_rows, &r, &c)) {
        r = 0;
        c = 0;
    }

    char buf[32];
    int n = snprintf(buf, sizeof(buf), "\x1b[%d;%dH", r + 1, c + 1);
    abAppend(&ab, buf, n);
    abAppend(&ab, "\x1b[?25h", 6);

    write(STDOUT_FILENO, ab.b, (size_t)ab.len);
    abFree(&ab);
}

/* ----- editing operations ------- */

static void editor_move_cursor(int key) {
    if (global_buffer.line_count == 0) return;
    if (global_cursor.row >= global_buffer.line_count) global_cursor.row = global_buffer.line_count - 1;

    Line *line = &global_buffer.lines[global_cursor.row];

    switch (key) {
        case ARROW_LEFT:
            if (global_cursor.col > 0) {
                global_cursor.col--;
            } else if (global_cursor.row > 0) {
                global_cursor.row--;
                global_cursor.col = global_buffer.lines[global_cursor.row].len;
            }
            break;
        
        case ARROW_RIGHT:
            if (global_cursor.col < line->len) {
                global_cursor.col++;
            } else if (global_cursor.row + 1 < global_buffer.line_count) {
                global_cursor.row++;
                global_cursor.col = 0;
            }
            break;

        case ARROW_UP:
            if (global_cursor.row > 0) global_cursor.row--;
            break;

        case ARROW_DOWN:
            if (global_cursor.row + 1 < global_buffer.line_count) global_cursor.row++;
            break;
    }

    line = &global_buffer.lines[global_cursor.row];
    if (global_cursor.col > line->len) global_cursor.col = line->len;
}

static void editor_insert_char(char c) {
    if (global_cursor.row >= global_buffer.line_count) return;
    Line *l = &global_buffer.lines[global_cursor.row];
    global_cursor.col = MIN(global_cursor.col, l->len);
    line_insert_char(l, global_cursor.col, c);
    global_cursor.col++;
    global_dirty = true;
    editor_update_syntax_from(global_cursor.row);
}

static void editor_insert_newline(void) {
    size_t start = global_cursor.row;
    buffer_split_line(&global_buffer, &global_cursor);
    global_dirty = true;
    editor_update_syntax_from(start);
}

static void editor_backspace(void) {
    if (global_cursor.row >= global_buffer.line_count) return;

    if (global_cursor.col > 0) {
        Line *l = &global_buffer.lines[global_cursor.row];
        line_delete_char(l, global_cursor.col - 1);
        global_cursor.col--;
        global_dirty = true;
        editor_update_syntax_from(global_cursor.row > 0 ? global_cursor.row - 1 : 0);
        return;
    }

    if (global_cursor.row > 0) {
        buffer_join_line_with_prev(&global_buffer, &global_cursor);
        global_dirty = true;
        editor_update_syntax_from(global_cursor.row > 0 ? global_cursor.row - 1 : 0);
    }
}

/* ------ command mode ------ */

static void editor_enter_command_mode(void) {
    global_mode = COMMAND;
    global_cmd_len = 0;
    global_cmd[0] = '\0';
}

static void editor_leave_command_mode(void) {
    global_mode = NORMAL;
    global_cmd_len = 0;
    global_cmd[0] = '\0';
}

static void editor_execute_command(void) {
    global_cmd[global_cmd_len] = '\0';

    char *cmd = global_cmd;
    while (*cmd == ' ') cmd++;

    if (strcmp(cmd, "q") == 0 || strcmp(cmd, "quit") == 0) {
        if (global_dirty) {
            snprintf(global_status, sizeof(global_status), "No write since last change (use :q!)");
        } else {
            editor_running = false;
        }
    } else if (strcmp(cmd, "q!") == 0) {
        editor_running = false;
    } else if (strcmp(cmd, "w") == 0) {
        dump_buffer_to_file(&global_buffer, global_filename);
    } else if (strncmp(cmd, "w ", 2) == 0) {
        cmd += 2;
        while (*cmd == ' ') cmd++;
        if (*cmd == '\0') {
            snprintf(global_status, sizeof(global_status), "Usage: :w <path>");
        } else {
            free(global_filename_owned);
            global_filename_owned = strdup(cmd);
            if (!global_filename_owned) die("strdup");
            global_filename = global_filename_owned;
            dump_buffer_to_file(&global_buffer, global_filename);
        }
    } else if (strcmp(cmd, "wq") == 0) {
        if (dump_buffer_to_file(&global_buffer, global_filename) == 0) {
            editor_running = false;
        }
	} else {
		char *endptr;
		strtol(cmd, &endptr, 10);

		if (*endptr == '\0') {
			global_cursor.row = atoi(cmd);
			global_cursor.col = 1;
            editor_refresh_screen();
		} else {
            snprintf(global_status, sizeof(global_status), "Unknown command: %s", cmd);
        }            
    }

    editor_leave_command_mode();
}

static void editor_command_keypress(int key) {
    if (key == ESC) {
        editor_leave_command_mode();
        return;
    }
    if (key == ENTER || key == '\r') {
        editor_execute_command();
        return;
    }
    if (key == BACKSPACE || key == DEL) {
        if (global_cmd_len > 0) {
            global_cmd_len--;
            global_cmd[global_cmd_len] = '\0';
        } else {
            editor_leave_command_mode();
        }
        return;
    }

    if (isprint(key) && global_cmd_len + 1 < sizeof(global_cmd)) {
        global_cmd[global_cmd_len++] = (char)key;
        global_cmd[global_cmd_len] = '\0';
    }
}

/* ---- main input loop ----- */

static void editor_process_keypress(void) {
    int key = editor_read_key();

    if (global_mode != COMMAND) global_status[0] = '\0';

    if (global_mode == COMMAND) {
        editor_command_keypress(key);
        return;
    }

    if (key == ARROW_UP || key == ARROW_DOWN || key == ARROW_LEFT || key == ARROW_RIGHT) {
        editor_move_cursor(key);
        return;
    }

    if (global_mode == INSERT) {
        if (key == ESC) {
            global_mode = NORMAL;
            return;
        }
        if (key == ENTER || key == '\r') {
            editor_insert_newline();
            return;
        }
        if (key == BACKSPACE || key == DEL) {
            editor_backspace();
            return;
        }
        if (key == TAB) {
            editor_insert_char('\t');
            return;
        }
        if (isprint(key)) {
            editor_insert_char((char)key);
            return;
        }
        return;
    }

    // NORMAL mode
    if (key == 'i') { global_mode = INSERT; return; }
    if (key == ':') { editor_enter_command_mode(); return; }
    if (key == ESC) { global_mode = NORMAL; return; }

    if (key == 'h') { editor_move_cursor(ARROW_LEFT); return; }
    if (key == 'j') { editor_move_cursor(ARROW_DOWN); return; }
    if (key == 'k') { editor_move_cursor(ARROW_UP); return; }
    if (key == 'l') { editor_move_cursor(ARROW_RIGHT); return; }

	if (key == 'x') { 
		line_delete_char(&global_buffer.lines[global_cursor.row], global_cursor.col);
		return;
	}

    if (key == 'd') {
        global_control_char = 'd';
        editor_refresh_screen();
        int other_key = editor_read_key();
        if (other_key == 'd') {
            global_control_char = ' ';
            buffer_delete_line(&global_buffer, global_cursor.row);
        } else {
            global_control_char = ' ';
            return;
        }
    }
}

int main(int argc, char *argv[]) {
    buffer_init(&global_buffer);    

    if (argc >= 2) {
        global_filename = argv[1];
        FILE *fp = fopen(global_filename, "r");
        if (fp) {
            buffer_load_file(&global_buffer, fp);
            global_dirty = false;
            editor_update_syntax_from(0);
        } else {
            global_dirty = false;
            snprintf(global_status, sizeof(global_status), "New file");
        }
    } else {
        global_filename = NULL;
        snprintf(global_status, sizeof(global_status), "No file (use :w <path>)");
    }

    global_cursor.row = 0;
    global_cursor.col = 0;
    global_view.top_line = 0;
    global_view.top_rowoff = 0;

    enable_raw_mode();
    write(STDOUT_FILENO, "\x1b[2J\x1b[H", 7);

    while (editor_running) {
        editor_refresh_screen();
        editor_process_keypress();
    }

    write(STDOUT_FILENO, "\x1b[2J\x1b[H\x1b[?25h", 13);
    buffer_free(&global_buffer);
    free(global_filename_owned);
    return 0;
}
