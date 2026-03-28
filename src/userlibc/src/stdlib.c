/*
 * MOSS user-space libc — stdlib (malloc/free/calloc/realloc + utilities).
 *
 * malloc uses a simple first-fit free-list allocator backed by sbrk().
 * Each allocation has an 8-byte header: [size | next_free*]
 */
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ------------------------------------------------------------------ */
/*  malloc / free / calloc / realloc                                   */
/* ------------------------------------------------------------------ */

/* Allocation header — 8 bytes, placed just before the returned pointer */
typedef struct block_header {
    size_t                size;  /* usable bytes (excludes header) */
    struct block_header  *next;  /* next in free list (only when free) */
} block_header_t;

#define HEADER_SIZE sizeof(block_header_t)
#define ALIGN16(x) (((x) + 15) & ~(size_t)15)
#define MIN_ALLOC  4096  /* minimum sbrk increment */

static block_header_t *free_list = NULL;

/* Extend the heap via sbrk and return a new block header */
static block_header_t *extend_heap(size_t size) {
    size_t total = HEADER_SIZE + size;
    if (total < MIN_ALLOC)
        total = MIN_ALLOC;

    void *p = sbrk((intptr_t)total);
    if (p == (void *)-1)
        return NULL;

    block_header_t *blk = (block_header_t *)p;
    blk->size = total - HEADER_SIZE;
    blk->next = NULL;
    return blk;
}

void *malloc(size_t size) {
    if (size == 0)
        return NULL;
    size = ALIGN16(size);

    /* First fit in free list */
    block_header_t **prev = &free_list;
    block_header_t *cur = free_list;

    while (cur) {
        if (cur->size >= size) {
            /* Split if remainder is large enough */
            if (cur->size >= size + HEADER_SIZE + 16) {
                block_header_t *remainder =
                    (block_header_t *)((char *)cur + HEADER_SIZE + size);
                remainder->size = cur->size - size - HEADER_SIZE;
                remainder->next = cur->next;
                cur->size = size;
                *prev = remainder;
            } else {
                *prev = cur->next;
            }
            cur->next = NULL;
            return (void *)((char *)cur + HEADER_SIZE);
        }
        prev = &cur->next;
        cur = cur->next;
    }

    /* No free block found — grow the heap */
    block_header_t *new_blk = extend_heap(size);
    if (!new_blk)
        return NULL;

    /* If the new block is bigger than needed, add remainder to free list */
    if (new_blk->size > size + HEADER_SIZE + 16) {
        block_header_t *remainder =
            (block_header_t *)((char *)new_blk + HEADER_SIZE + size);
        remainder->size = new_blk->size - size - HEADER_SIZE;
        remainder->next = free_list;
        free_list = remainder;
        new_blk->size = size;
    }

    return (void *)((char *)new_blk + HEADER_SIZE);
}

void free(void *ptr) {
    if (!ptr)
        return;

    block_header_t *blk = (block_header_t *)((char *)ptr - HEADER_SIZE);

    /* Insert at head of free list (simple, no coalescing) */
    blk->next = free_list;
    free_list = blk;
}

void *calloc(size_t nmemb, size_t size) {
    size_t total = nmemb * size;
    void *p = malloc(total);
    if (p)
        memset(p, 0, total);
    return p;
}

void *realloc(void *ptr, size_t size) {
    if (!ptr)
        return malloc(size);
    if (size == 0) {
        free(ptr);
        return NULL;
    }

    block_header_t *blk = (block_header_t *)((char *)ptr - HEADER_SIZE);
    if (blk->size >= size)
        return ptr;  /* existing block is big enough */

    void *new_ptr = malloc(size);
    if (!new_ptr)
        return NULL;
    memcpy(new_ptr, ptr, blk->size);
    free(ptr);
    return new_ptr;
}

/* ------------------------------------------------------------------ */
/*  atexit                                                             */
/* ------------------------------------------------------------------ */

#define ATEXIT_MAX 32
static void (*atexit_funcs[ATEXIT_MAX])(void);
static int atexit_count = 0;

int atexit(void (*func)(void)) {
    if (atexit_count >= ATEXIT_MAX)
        return -1;
    atexit_funcs[atexit_count++] = func;
    return 0;
}

/* ------------------------------------------------------------------ */
/*  Utility functions                                                  */
/* ------------------------------------------------------------------ */

void exit(int status) {
    for (int i = atexit_count - 1; i >= 0; i--)
        atexit_funcs[i]();
    _exit(status);
}

void abort(void) {
    _exit(127);
}

int atoi(const char *s) {
    int sign = 1, val = 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9')
        val = val * 10 + (*s++ - '0');
    return val * sign;
}

long atol(const char *s) {
    return (long)atoi(s);
}

double atof(const char *s) {
    double result = 0.0;
    double frac = 0.0;
    int sign = 1;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;
    while (*s >= '0' && *s <= '9')
        result = result * 10.0 + (*s++ - '0');
    if (*s == '.') {
        s++;
        double place = 0.1;
        while (*s >= '0' && *s <= '9') {
            frac += (*s++ - '0') * place;
            place *= 0.1;
        }
    }
    return sign * (result + frac);
}

int system(const char *command) {
    (void)command;
    return -1;  /* no shell in MOSS user-space */
}

int abs(int x) {
    return x < 0 ? -x : x;
}

long labs(long x) {
    return x < 0 ? -x : x;
}

/* ------------------------------------------------------------------ */
/*  strtod / strtof                                                    */
/* ------------------------------------------------------------------ */

double strtod(const char *nptr, char **endptr) {
    const char *s = nptr;
    double result = 0.0;
    int sign = 1;

    /* Skip whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;

    /* Sign */
    if (*s == '-') { sign = -1; s++; }
    else if (*s == '+') s++;

    const char *start = s;

    /* Integer part */
    while (*s >= '0' && *s <= '9')
        result = result * 10.0 + (*s++ - '0');

    /* Fractional part */
    if (*s == '.') {
        s++;
        double place = 0.1;
        while (*s >= '0' && *s <= '9') {
            result += (*s++ - '0') * place;
            place *= 0.1;
        }
    }

    /* Exponent */
    if (*s == 'e' || *s == 'E') {
        s++;
        int exp_sign = 1;
        int exp_val = 0;
        if (*s == '-') { exp_sign = -1; s++; }
        else if (*s == '+') s++;
        while (*s >= '0' && *s <= '9')
            exp_val = exp_val * 10 + (*s++ - '0');
        double factor = 1.0;
        for (int i = 0; i < exp_val; i++)
            factor *= 10.0;
        if (exp_sign < 0)
            result /= factor;
        else
            result *= factor;
    }

    if (endptr)
        *endptr = (char *)(s == start ? nptr : s);
    return sign * result;
}

float strtof(const char *nptr, char **endptr) {
    return (float)strtod(nptr, endptr);
}

/* ------------------------------------------------------------------ */
/*  strtol / strtoul                                                   */
/* ------------------------------------------------------------------ */

unsigned long strtoul(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long result = 0;

    /* Skip leading whitespace */
    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;

    /* Optional sign (strtoul accepts + but not -) */
    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    /* Auto-detect base from prefix */
    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') { base = 16; s++; }
            else base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            s += 2;
    }

    const char *start = s;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9')
            digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z')
            digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z')
            digit = *s - 'A' + 10;
        else
            break;
        if (digit >= base)
            break;
        result = result * (unsigned long)base + (unsigned long)digit;
        s++;
    }

    if (endptr)
        *endptr = (char *)(s == start ? nptr : s);
    return neg ? (unsigned long)(-(long)result) : result;
}

long strtol(const char *nptr, char **endptr, int base) {
    const char *s = nptr;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;

    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    /* Reparse from the sign-stripped position */
    char *end;
    unsigned long val = strtoul(s, &end, base);

    if (endptr)
        *endptr = (end == s) ? (char *)nptr : end;

    return neg ? -(long)val : (long)val;
}

/* ------------------------------------------------------------------ */
/*  getenv — stub (no environment in MOSS)                             */
/* ------------------------------------------------------------------ */

char *getenv(const char *name) {
    (void)name;
    return (char *)0;
}

/* ------------------------------------------------------------------ */
/*  qsort — simple insertion sort for small arrays                     */
/* ------------------------------------------------------------------ */

void qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *)) {
    char *arr = (char *)base;
    char tmp[256]; /* for elements up to 256 bytes; sufficient for typical use */

    for (size_t i = 1; i < nmemb; i++) {
        memcpy(tmp, arr + i * size, size);
        size_t j = i;
        while (j > 0 && compar(arr + (j - 1) * size, tmp) > 0) {
            memcpy(arr + j * size, arr + (j - 1) * size, size);
            j--;
        }
        memcpy(arr + j * size, tmp, size);
    }
}

/* ------------------------------------------------------------------ */
/*  strtold / strtoll / strtoull                                       */
/* ------------------------------------------------------------------ */

long double strtold(const char *nptr, char **endptr) {
    return (long double)strtod(nptr, endptr);
}

unsigned long long strtoull(const char *nptr, char **endptr, int base) {
    const char *s = nptr;
    unsigned long long result = 0;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;

    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    if (base == 0) {
        if (*s == '0') {
            s++;
            if (*s == 'x' || *s == 'X') { base = 16; s++; }
            else base = 8;
        } else {
            base = 10;
        }
    } else if (base == 16) {
        if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
            s += 2;
    }

    const char *start = s;
    while (*s) {
        int digit;
        if (*s >= '0' && *s <= '9')
            digit = *s - '0';
        else if (*s >= 'a' && *s <= 'z')
            digit = *s - 'a' + 10;
        else if (*s >= 'A' && *s <= 'Z')
            digit = *s - 'A' + 10;
        else
            break;
        if (digit >= base)
            break;
        result = result * (unsigned long long)base + (unsigned long long)digit;
        s++;
    }

    if (endptr)
        *endptr = (char *)(s == start ? nptr : s);
    return neg ? (unsigned long long)(-(long long)result) : result;
}

long long strtoll(const char *nptr, char **endptr, int base) {
    const char *s = nptr;

    while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r')
        s++;

    int neg = 0;
    if (*s == '-') { neg = 1; s++; }
    else if (*s == '+') s++;

    char *end;
    unsigned long long val = strtoull(s, &end, base);

    if (endptr)
        *endptr = (end == s) ? (char *)nptr : end;

    return neg ? -(long long)val : (long long)val;
}

/* ------------------------------------------------------------------ */
/*  realpath — simplified (no symlink support)                         */
/* ------------------------------------------------------------------ */

char *realpath(const char *path, char *resolved) {
    if (!path)
        return (char *)0;

    char *buf = resolved;
    if (!buf) {
        buf = (char *)malloc(256);
        if (!buf)
            return (char *)0;
    }

    if (path[0] == '/') {
        /* Absolute path — copy as-is (no symlinks in MOSS) */
        size_t len = strlen(path);
        if (len > 255) len = 255;
        memcpy(buf, path, len);
        buf[len] = '\0';
    } else {
        /* Relative path — prepend cwd */
        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd))) {
            if (!resolved) free(buf);
            return (char *)0;
        }
        size_t clen = strlen(cwd);
        size_t plen = strlen(path);
        if (clen + 1 + plen > 255) {
            if (!resolved) free(buf);
            return (char *)0;
        }
        memcpy(buf, cwd, clen);
        if (clen > 1) buf[clen++] = '/';
        memcpy(buf + clen, path, plen);
        buf[clen + plen] = '\0';
    }
    return buf;
}
