/*
 * MOSS user-space libc — <stdlib.h>
 */
#ifndef _STDLIB_H
#define _STDLIB_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t size);
void  free(void *ptr);
void *calloc(size_t nmemb, size_t size);
void *realloc(void *ptr, size_t size);

void exit(int status) __attribute__((noreturn));
void abort(void) __attribute__((noreturn));

int  atoi(const char *s);
long atol(const char *s);
double atof(const char *s);
long strtol(const char *nptr, char **endptr, int base);
unsigned long strtoul(const char *nptr, char **endptr, int base);
double strtod(const char *nptr, char **endptr);
float  strtof(const char *nptr, char **endptr);
long double strtold(const char *nptr, char **endptr);
long long strtoll(const char *nptr, char **endptr, int base);
unsigned long long strtoull(const char *nptr, char **endptr, int base);
int  abs(int x);
long labs(long x);

char *getenv(const char *name);
int   setenv(const char *name, const char *value, int overwrite);
int   unsetenv(const char *name);
int   putenv(char *string);
int   system(const char *command);
char *realpath(const char *path, char *resolved);

extern char **environ;

void qsort(void *base, size_t nmemb, size_t size,
            int (*compar)(const void *, const void *));

int atexit(void (*func)(void));

#define RAND_MAX 2147483647
int   rand(void);
void  srand(unsigned int seed);

#ifdef __cplusplus
}
#endif

#endif /* _STDLIB_H */
