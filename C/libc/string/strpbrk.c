#include<string.h>

char *strpbrk(const char *str, const char *delimiters) {
  while (*str) {
    const char *d = delimiters;
    while (*d) {
      if (*d == *str) {
        return (char *)str;
      }
      ++d;
    }
    ++str;
  }
  return NULL;
}
