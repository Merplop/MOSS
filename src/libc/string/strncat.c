#include <string.h>

char* strncat(char* restrict dest, const char* restrict src, size_t n) {
	char* ret = dest;
	while (*dest)
		dest++;
	while (n-- && (*dest = *src)) {
		dest++;
		src++;
	}
	*dest = '\0';
	return ret;
}
