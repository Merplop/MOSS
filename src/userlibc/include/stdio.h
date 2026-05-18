/*
 * MOSS user-space libc — <stdio.h>
 * Standard I/O functions.
 */
#ifndef _STDIO_H
#define _STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EOF (-1)
#define BUFSIZ 1024

/* Seek constants (also in unistd.h / syscall.h) */
#ifndef SEEK_SET
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2
#endif

/* Internal read buffer size */
#define _STDIO_RBUF_SIZE 4096

typedef struct _FILE {
    int   fd;
    int   eof;
    int   error;
    /* Read buffer for reducing syscall overhead */
    unsigned char rbuf[_STDIO_RBUF_SIZE];
    size_t rbuf_pos;   /* next byte to consume */
    size_t rbuf_len;   /* valid bytes in buffer */
} FILE;

extern FILE *stdin;
extern FILE *stdout;
extern FILE *stderr;

#define FILENAME_MAX 256

FILE *fopen(const char *path, const char *mode);
FILE *fdopen(int fd, const char *mode);
int   fclose(FILE *stream);
size_t fread(void *ptr, size_t size, size_t nmemb, FILE *stream);
size_t fwrite(const void *ptr, size_t size, size_t nmemb, FILE *stream);
int   fseek(FILE *stream, long offset, int whence);
long  ftell(FILE *stream);
int   feof(FILE *stream);
int   ferror(FILE *stream);
void  clearerr(FILE *stream);
int   fflush(FILE *stream);

int   fgetc(FILE *stream);
#define getc(stream) fgetc(stream)
int   fputc(int c, FILE *stream);
char *fgets(char *s, int size, FILE *stream);
int   fputs(const char *s, FILE *stream);

int printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
int fprintf(FILE *stream, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int sprintf(char *str, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
int snprintf(char *str, size_t size, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
int vsnprintf(char *str, size_t size, const char *fmt, va_list ap);
int vsprintf(char *str, const char *fmt, va_list ap);
int vfprintf(FILE *stream, const char *fmt, va_list ap);
int vprintf(const char *fmt, va_list ap);
int puts(const char *s);
int putchar(int c);

int rename(const char *oldpath, const char *newpath);
int remove(const char *path);

int sscanf(const char *str, const char *fmt, ...);
int fscanf(FILE *stream, const char *fmt, ...);

void perror(const char *s);
ssize_t getline(char **lineptr, size_t *n, FILE *stream);

#ifdef __cplusplus
}
#endif

#endif /* _STDIO_H */
