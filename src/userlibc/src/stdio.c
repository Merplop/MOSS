/*
 * MOSS user-space libc — stdio implementation.
 * Provides fopen/fclose/fread/fwrite/fseek/ftell/printf/sprintf etc.
 */
#include <stdio.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <fcntl.h>
#include <stdarg.h>
#include <errno.h>

/* ------------------------------------------------------------------ */
/*  Static FILE objects for stdin/stdout/stderr                        */
/* ------------------------------------------------------------------ */

static FILE _stdin  = { .fd = 0, .eof = 0, .error = 0, .rbuf_pos = 0, .rbuf_len = 0 };
static FILE _stdout = { .fd = 1, .eof = 0, .error = 0, .rbuf_pos = 0, .rbuf_len = 0 };
static FILE _stderr = { .fd = 2, .eof = 0, .error = 0, .rbuf_pos = 0, .rbuf_len = 0 };

FILE *stdin  = &_stdin;
FILE *stdout = &_stdout;
FILE *stderr = &_stderr;

/* Small pool of FILE structs for fopen (no malloc dependency for basic use) */
#define MAX_FILES 16
static FILE file_pool[MAX_FILES];
static int  file_pool_used[MAX_FILES];

static FILE *alloc_file(void) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (!file_pool_used[i]) {
            file_pool_used[i] = 1;
            file_pool[i].eof = 0;
            file_pool[i].error = 0;
            file_pool[i].rbuf_pos = 0;
            file_pool[i].rbuf_len = 0;
            return &file_pool[i];
        }
    }
    return NULL;
}

static void free_file(FILE *f) {
    for (int i = 0; i < MAX_FILES; i++) {
        if (&file_pool[i] == f) {
            file_pool_used[i] = 0;
            return;
        }
    }
}

/* ------------------------------------------------------------------ */
/*  fopen / fclose / fread / fwrite / fseek / ftell                    */
/* ------------------------------------------------------------------ */

FILE *fdopen(int fd, const char *mode) {
    (void)mode; /* mode is advisory since fd is already open */
    if (fd < 0)
        return NULL;
    FILE *f = alloc_file();
    if (!f)
        return NULL;
    f->fd = fd;
    return f;
}

FILE *fopen(const char *path, const char *mode) {
    int flags = 0;

    if (mode[0] == 'r') {
        if (mode[1] == '+')
            flags = O_RDWR;
        else
            flags = O_RDONLY;
    } else if (mode[0] == 'w') {
        if (mode[1] == '+')
            flags = O_RDWR | O_CREAT | O_TRUNC;
        else
            flags = O_WRONLY | O_CREAT | O_TRUNC;
    } else if (mode[0] == 'a') {
        flags = O_WRONLY | O_CREAT;
    } else {
        return NULL;
    }

    int fd = open(path, flags);
    if (fd < 0)
        return NULL;

    FILE *f = alloc_file();
    if (!f) {
        close(fd);
        return NULL;
    }
    f->fd = fd;

    /* For append mode, seek to end */
    if (mode[0] == 'a')
        lseek(fd, 0, SEEK_END);

    return f;
}

int fclose(FILE *stream) {
    if (!stream)
        return EOF;
    int rc = close(stream->fd);
    free_file(stream);
    return rc;
}

size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream || size == 0 || nmemb == 0)
        return 0;

    size_t total = size * nmemb;
    size_t done = 0;
    char *dst = (char *)ptr;

    while (done < total) {
        /* Serve from read buffer first */
        if (stream->rbuf_pos < stream->rbuf_len) {
            size_t avail = stream->rbuf_len - stream->rbuf_pos;
            size_t want = total - done;
            size_t chunk = (avail < want) ? avail : want;
            memcpy(dst + done, stream->rbuf + stream->rbuf_pos, chunk);
            stream->rbuf_pos += chunk;
            done += chunk;
            continue;
        }

        /* Buffer empty — refill */
        ssize_t n = read(stream->fd, stream->rbuf, _STDIO_RBUF_SIZE);
        if (n < 0) {
            stream->error = 1;
            break;
        }
        if (n == 0) {
            stream->eof = 1;
            break;
        }
        stream->rbuf_pos = 0;
        stream->rbuf_len = (size_t)n;
    }

    return done / size;
}

size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream) {
    if (!stream || size == 0 || nmemb == 0)
        return 0;

    size_t total = size * nmemb;
    size_t done = 0;
    const char *src = (const char *)ptr;

    while (done < total) {
        ssize_t n = write(stream->fd, src + done, total - done);
        if (n <= 0) {
            stream->error = 1;
            break;
        }
        done += (size_t)n;
    }

    return done / size;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream)
        return -1;
    /* Invalidate read buffer */
    stream->rbuf_pos = 0;
    stream->rbuf_len = 0;
    off_t result = lseek(stream->fd, (off_t)offset, whence);
    if (result < 0)
        return -1;
    stream->eof = 0;
    return 0;
}

long ftell(FILE *stream) {
    if (!stream)
        return -1;
    long pos = (long)lseek(stream->fd, 0, SEEK_CUR);
    if (pos < 0)
        return -1;
    /* Subtract buffered bytes not yet consumed */
    pos -= (long)(stream->rbuf_len - stream->rbuf_pos);
    return pos;
}

int feof(FILE *stream) {
    return stream ? stream->eof : 0;
}

int ferror(FILE *stream) {
    return stream ? stream->error : 0;
}

void clearerr(FILE *stream) {
    if (stream) {
        stream->eof = 0;
        stream->error = 0;
    }
}

int fgetc(FILE *stream) {
    unsigned char c;
    if (fread(&c, 1, 1, stream) != 1)
        return EOF;
    return (int)c;
}

int fputc(int c, FILE *stream) {
    unsigned char ch = (unsigned char)c;
    if (fwrite(&ch, 1, 1, stream) != 1)
        return EOF;
    return (int)ch;
}

char *fgets(char *s, int size, FILE *stream) {
    if (!s || size <= 0 || !stream)
        return NULL;
    int i = 0;
    while (i < size - 1) {
        int c = fgetc(stream);
        if (c == EOF) {
            if (i == 0) return NULL;
            break;
        }
        s[i++] = (char)c;
        if (c == '\n') break;
    }
    s[i] = '\0';
    return s;
}

int fputs(const char *s, FILE *stream) {
    size_t len = strlen(s);
    if (fwrite(s, 1, len, stream) != len)
        return EOF;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  vsnprintf — the core printf engine                                 */
/* ------------------------------------------------------------------ */

/* Helper: write a character to the buffer if there's room */
static void buf_putc(char *buf, size_t size, size_t *pos, char c) {
    if (*pos < size - 1)
        buf[*pos] = c;
    (*pos)++;
}

/* Helper: write a string to the buffer */
static void buf_puts(char *buf, size_t size, size_t *pos, const char *s) {
    while (*s)
        buf_putc(buf, size, pos, *s++);
}

/* Helper: write an unsigned integer in given base */
static void buf_put_uint(char *buf, size_t size, size_t *pos, unsigned long val,
                         int base, int width, char pad, int uppercase) {
    char tmp[32];
    const char *digits = uppercase ? "0123456789ABCDEF" : "0123456789abcdef";
    int len = 0;

    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val > 0) {
            tmp[len++] = digits[val % base];
            val /= base;
        }
    }

    /* Padding */
    for (int i = len; i < width; i++)
        buf_putc(buf, size, pos, pad);

    /* Reverse the digits */
    for (int i = len - 1; i >= 0; i--)
        buf_putc(buf, size, pos, tmp[i]);
}

int vsnprintf(char *str, size_t size, const char *fmt, va_list ap) {
    if (size == 0) return 0;

    size_t pos = 0;

    while (*fmt) {
        if (*fmt != '%') {
            buf_putc(str, size, &pos, *fmt++);
            continue;
        }

        fmt++;  /* skip '%' */

        /* Flags */
        char pad = ' ';
        int left_align = 0;
        if (*fmt == '-') { left_align = 1; fmt++; }
        if (*fmt == '0') { pad = '0'; fmt++; }

        /* Width */
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt - '0');
                fmt++;
            }
        }

        /* Precision */
        int precision = -1;  /* -1 = not specified */
        if (*fmt == '.') {
            fmt++;
            precision = 0;
            if (*fmt == '*') {
                precision = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    precision = precision * 10 + (*fmt - '0');
                    fmt++;
                }
            }
        }

        /* Length modifier */
        int is_long = 0;
        if (*fmt == 'l') { is_long = 1; fmt++; }
        if (*fmt == 'l') { is_long = 2; fmt++; }  /* ll */

        /* Conversion */
        switch (*fmt) {
        case 'd':
        case 'i': {
            long val = is_long ? va_arg(ap, long) : (long)va_arg(ap, int);
            int neg = 0;
            if (val < 0) {
                neg = 1;
                val = -val;
            }
            /* If precision is given, it sets the minimum digits (zero-padded),
               and the pad char for width should be space (not '0'). */
            int min_digits = 0;
            if (precision >= 0) {
                min_digits = precision;
                pad = ' ';  /* width padding is always space when precision set */
            }
            /* Calculate the number of digits */
            char tmp_d[32];
            const char *digs = "0123456789";
            int dlen = 0;
            unsigned long uv = (unsigned long)val;
            if (uv == 0) { tmp_d[dlen++] = '0'; }
            else { while (uv > 0) { tmp_d[dlen++] = digs[uv % 10]; uv /= 10; } }
            int num_len = dlen < min_digits ? min_digits : dlen;
            int total = num_len + neg;
            if (!left_align) {
                for (int i = total; i < width; i++)
                    buf_putc(str, size, &pos, pad);
            }
            if (neg) buf_putc(str, size, &pos, '-');
            for (int i = dlen; i < min_digits; i++)
                buf_putc(str, size, &pos, '0');
            for (int i = dlen - 1; i >= 0; i--)
                buf_putc(str, size, &pos, tmp_d[i]);
            if (left_align) {
                for (int i = total; i < width; i++)
                    buf_putc(str, size, &pos, ' ');
            }
            break;
        }
        case 'u': {
            unsigned long val = is_long ?
                va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            int min_digits = (precision >= 0) ? precision : 0;
            if (precision >= 0) pad = ' ';
            buf_put_uint(str, size, &pos, val, 10,
                         min_digits > width ? min_digits : width,
                         precision >= 0 ? '0' : pad, 0);
            break;
        }
        case 'x': {
            unsigned long val = is_long ?
                va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            int min_digits = (precision >= 0) ? precision : 0;
            if (precision >= 0) pad = ' ';
            buf_put_uint(str, size, &pos, val, 16,
                         min_digits > width ? min_digits : width,
                         precision >= 0 ? '0' : pad, 0);
            break;
        }
        case 'X': {
            unsigned long val = is_long ?
                va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            int min_digits = (precision >= 0) ? precision : 0;
            if (precision >= 0) pad = ' ';
            buf_put_uint(str, size, &pos, val, 16,
                         min_digits > width ? min_digits : width,
                         precision >= 0 ? '0' : pad, 1);
            break;
        }
        case 'o': {
            unsigned long val = is_long ?
                va_arg(ap, unsigned long) : (unsigned long)va_arg(ap, unsigned int);
            int min_digits = (precision >= 0) ? precision : 0;
            if (precision >= 0) pad = ' ';
            buf_put_uint(str, size, &pos, val, 8,
                         min_digits > width ? min_digits : width,
                         precision >= 0 ? '0' : pad, 0);
            break;
        }
        case 'p': {
            unsigned long val = (unsigned long)va_arg(ap, void *);
            buf_puts(str, size, &pos, "0x");
            buf_put_uint(str, size, &pos, val, 16, 8, '0', 0);
            break;
        }
        case 'f': {
            double val = va_arg(ap, double);
            int neg = 0;
            if (val < 0) { neg = 1; val = -val; }
            int frac_digits = (precision >= 0) ? precision : 6;
            /* Integer part */
            unsigned long ipart = (unsigned long)val;
            /* Fractional part: multiply by 10^frac_digits and round */
            double frac = val - (double)ipart;
            unsigned long fpart = 0;
            double mult = 1.0;
            for (int fi = 0; fi < frac_digits; fi++) mult *= 10.0;
            fpart = (unsigned long)(frac * mult + 0.5);
            /* Handle rounding overflow (e.g. 0.9999... → 1.0) */
            unsigned long fmax = (unsigned long)mult;
            if (fpart >= fmax) { fpart = 0; ipart++; }
            /* Compute lengths for width padding */
            char itmp[32]; int ilen = 0;
            { unsigned long v = ipart;
              if (v == 0) itmp[ilen++] = '0';
              else while (v > 0) { itmp[ilen++] = '0' + (v % 10); v /= 10; }
            }
            int total = neg + ilen + (frac_digits > 0 ? 1 + frac_digits : 0);
            if (!left_align)
                for (int fi = total; fi < width; fi++)
                    buf_putc(str, size, &pos, pad);
            if (neg) buf_putc(str, size, &pos, '-');
            for (int fi = ilen - 1; fi >= 0; fi--)
                buf_putc(str, size, &pos, itmp[fi]);
            if (frac_digits > 0) {
                buf_putc(str, size, &pos, '.');
                /* Print fractional digits with leading zeros */
                char ftmp[32]; int flen = 0;
                if (fpart == 0) { ftmp[flen++] = '0'; }
                else { unsigned long fv = fpart;
                       while (fv > 0) { ftmp[flen++] = '0' + (fv % 10); fv /= 10; } }
                for (int fi = flen; fi < frac_digits; fi++)
                    buf_putc(str, size, &pos, '0');
                for (int fi = flen - 1; fi >= 0; fi--)
                    buf_putc(str, size, &pos, ftmp[fi]);
            }
            if (left_align)
                for (int fi = total; fi < width; fi++)
                    buf_putc(str, size, &pos, ' ');
            break;
        }
        case 'c': {
            char c = (char)va_arg(ap, int);
            buf_putc(str, size, &pos, c);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (!s) s = "(null)";
            int slen = (int)strlen(s);
            if (precision >= 0 && slen > precision)
                slen = precision;
            if (!left_align) {
                for (int i = slen; i < width; i++)
                    buf_putc(str, size, &pos, ' ');
            }
            for (int i = 0; i < slen; i++)
                buf_putc(str, size, &pos, s[i]);
            if (left_align) {
                for (int i = slen; i < width; i++)
                    buf_putc(str, size, &pos, ' ');
            }
            break;
        }
        case '%':
            buf_putc(str, size, &pos, '%');
            break;
        case '\0':
            goto done;
        default:
            buf_putc(str, size, &pos, '%');
            buf_putc(str, size, &pos, *fmt);
            break;
        }
        fmt++;
    }
done:
    if (pos < size)
        str[pos] = '\0';
    else
        str[size - 1] = '\0';
    return (int)pos;
}

int snprintf(char *str, size_t size, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(str, size, fmt, ap);
    va_end(ap);
    return ret;
}

int sprintf(char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(str, 0x7FFFFFFF, fmt, ap);
    va_end(ap);
    return ret;
}

int printf(const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (ret > 0)
        write(1, buf, (size_t)ret);
    return ret;
}

int fprintf(FILE *stream, const char *fmt, ...) {
    char buf[1024];
    va_list ap;
    va_start(ap, fmt);
    int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    if (ret > 0 && stream)
        write(stream->fd, buf, (size_t)ret);
    return ret;
}

int vsprintf(char *str, const char *fmt, va_list ap) {
    return vsnprintf(str, 0x7FFFFFFF, fmt, ap);
}

int vprintf(const char *fmt, va_list ap) {
    return vfprintf(stdout, fmt, ap);
}

int vfprintf(FILE *stream, const char *fmt, va_list ap) {
    char buf[1024];
    int ret = vsnprintf(buf, sizeof(buf), fmt, ap);
    if (ret > 0 && stream)
        write(stream->fd, buf, (size_t)ret);
    return ret;
}

int rename(const char *oldpath, const char *newpath) {
    (void)oldpath; (void)newpath;
    return -1;  /* not supported */
}

int remove(const char *path) {
    return unlink(path);
}

int fflush(FILE *stream) {
    (void)stream;
    return 0;  /* unbuffered I/O, nothing to flush */
}

int puts(const char *s) {
    int ret = (int)write(1, s, strlen(s));
    write(1, "\n", 1);
    return ret;
}

int putchar(int c) {
    char ch = (char)c;
    write(1, &ch, 1);
    return c;
}

/* ------------------------------------------------------------------ */
/*  vsscanf / sscanf / fscanf — minimal scan (%d, %i, %x, %s, %c, %u, %f) */
/* ------------------------------------------------------------------ */

static int is_space(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static int vsscanf(const char *str, const char *fmt, va_list ap) {
    int matched = 0;
    const char *p = str;

    while (*fmt && *p) {
        if (is_space(*fmt)) {
            fmt++;
            while (is_space(*p)) p++;
            continue;
        }
        if (*fmt != '%') {
            if (*fmt != *p) break;
            fmt++; p++;
            continue;
        }
        fmt++; /* skip % */

        /* optional field width */
        int width = 0;
        while (*fmt >= '0' && *fmt <= '9')
            width = width * 10 + (*fmt++ - '0');

        if (*fmt == 'd' || *fmt == 'i') {
            int *out = va_arg(ap, int *);
            int sign = 1;
            if (*p == '-') { sign = -1; p++; }
            else if (*p == '+') p++;
            if (*p < '0' || *p > '9') break;
            int val = 0;
            while (*p >= '0' && *p <= '9') val = val * 10 + (*p++ - '0');
            *out = val * sign;
            matched++;
        } else if (*fmt == 'u') {
            unsigned int *out = va_arg(ap, unsigned int *);
            if (*p < '0' || *p > '9') break;
            unsigned int val = 0;
            while (*p >= '0' && *p <= '9') val = val * 10 + (*p++ - '0');
            *out = val;
            matched++;
        } else if (*fmt == 'f') {
            float *out = va_arg(ap, float *);
            double sign = 1.0;
            if (*p == '-') { sign = -1.0; p++; }
            else if (*p == '+') p++;
            if ((*p < '0' || *p > '9') && *p != '.') break;
            double val = 0.0;
            while (*p >= '0' && *p <= '9') val = val * 10.0 + (*p++ - '0');
            if (*p == '.') {
                p++;
                double frac = 0.1;
                while (*p >= '0' && *p <= '9') {
                    val += (*p++ - '0') * frac;
                    frac *= 0.1;
                }
            }
            *out = (float)(val * sign);
            matched++;
        } else if (*fmt == 'x' || *fmt == 'X') {
            unsigned int *out = va_arg(ap, unsigned int *);
            if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
            unsigned int val = 0;
            while (1) {
                if (*p >= '0' && *p <= '9') val = val * 16 + (*p - '0');
                else if (*p >= 'a' && *p <= 'f') val = val * 16 + (*p - 'a' + 10);
                else if (*p >= 'A' && *p <= 'F') val = val * 16 + (*p - 'A' + 10);
                else break;
                p++;
            }
            *out = val;
            matched++;
        } else if (*fmt == 'c') {
            char *out = va_arg(ap, char *);
            *out = *p++;
            matched++;
        } else if (*fmt == 's') {
            char *out = va_arg(ap, char *);
            int n = 0;
            while (*p && !is_space(*p)) {
                if (width > 0 && n >= width) break;
                *out++ = *p++;
                n++;
            }
            *out = '\0';
            matched++;
        }
        fmt++;
    }
    return matched;
}

int sscanf(const char *str, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int ret = vsscanf(str, fmt, ap);
    va_end(ap);
    return ret;
}

int fscanf(FILE *stream, const char *fmt, ...) {
    if (!stream) return -1;
    /* Read a line from the stream into a buffer, then sscanf it */
    char buf[1024];
    int i = 0;
    int c;
    while (i < (int)sizeof(buf) - 1) {
        c = fgetc(stream);
        if (c == EOF) break;
        buf[i++] = (char)c;
        if (c == '\n') break;
    }
    if (i == 0) return -1;
    buf[i] = '\0';
    va_list ap;
    va_start(ap, fmt);
    int ret = vsscanf(buf, fmt, ap);
    va_end(ap);
    return ret;
}

void perror(const char *s) {
    if (s && *s) {
        fputs(s, stderr);
        fputs(": ", stderr);
    }
    fputs(strerror(errno), stderr);
    fputc('\n', stderr);
}

ssize_t getline(char **lineptr, size_t *n, FILE *stream) {
    if (!lineptr || !n || !stream)
        return -1;

    if (*lineptr == NULL || *n == 0) {
        *n = 128;
        *lineptr = malloc(*n);
        if (!*lineptr)
            return -1;
    }

    size_t pos = 0;
    int c;

    while ((c = fgetc(stream)) != EOF) {
        if (pos + 2 > *n) {
            size_t new_n = *n * 2;
            char *new_ptr = realloc(*lineptr, new_n);
            if (!new_ptr)
                return -1;
            *lineptr = new_ptr;
            *n = new_n;
        }
        (*lineptr)[pos++] = (char)c;
        if (c == '\n')
            break;
    }

    if (pos == 0)
        return -1;

    (*lineptr)[pos] = '\0';
    return (ssize_t)pos;
}
