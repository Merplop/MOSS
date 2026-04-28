#include <limits.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

static bool print(const char* data, size_t length) {
	const unsigned char* bytes = (const unsigned char*) data;
	for (size_t i = 0; i < length; i++)
		if (putchar(bytes[i]) == EOF)
			return false;
	return true;
}

static bool print_char(char c) {
	return print(&c, 1);
}

int print_hex(uint64_t hex_value) {
    uint8_t hexString[] = "0x0000000000000000\0";
    const char *ascii_numbers = "0123456789ABCDEF";
    uint8_t nibble;

    for (uint8_t i = 0; i < 16; i++) {
        nibble = (uint8_t)(hex_value & 0x0F);
        hexString[17-i] = ascii_numbers[nibble];
        hex_value >>= 4;
    }
    printf("%s", hexString);
    return 0;
}

/* Helper: render an unsigned value in the given base into tmp[],
 * returning the number of digits written. */
static int render_unsigned(char *tmp, size_t tmp_size, unsigned int val,
                           int base, int uppercase) {
    const char *digits = uppercase ? "0123456789ABCDEF"
                                   : "0123456789abcdef";
    int len = 0;
    if (val == 0) {
        tmp[len++] = '0';
    } else {
        while (val > 0 && len < (int)tmp_size - 1) {
            tmp[len++] = digits[val % base];
            val /= base;
        }
    }
    return len;
}

int printf(const char* restrict format, ...) {
	va_list parameters;
	va_start(parameters, format);

	int written = 0;

	while (*format != '\0') {
		/* Literal text run */
		if (format[0] != '%' || format[1] == '%') {
			if (format[0] == '%')
				format++;
			size_t amount = 1;
			while (format[amount] && format[amount] != '%')
				amount++;
			if (!print(format, amount))
				return -1;
			format += amount;
			written += amount;
			continue;
		}

		format++;  /* skip '%' */

		/* --- Parse flags --- */
		char pad = ' ';
		int left_align = 0;
		for (;;) {
			if (*format == '0')      { pad = '0'; format++; }
			else if (*format == '-') { left_align = 1; format++; }
			else break;
		}
		if (left_align) pad = ' ';  /* '-' overrides '0' */

		/* --- Parse width --- */
		int width = 0;
		if (*format == '*') {
			width = va_arg(parameters, int);
			format++;
		} else {
			while (*format >= '0' && *format <= '9') {
				width = width * 10 + (*format - '0');
				format++;
			}
		}

		/* --- Parse precision --- */
		int precision = -1;  /* -1 = not specified */
		if (*format == '.') {
			format++;
			precision = 0;
			if (*format == '*') {
				precision = va_arg(parameters, int);
				format++;
			} else {
				while (*format >= '0' && *format <= '9') {
					precision = precision * 10 + (*format - '0');
					format++;
				}
			}
		}

		/* --- Length modifier (unused in kernel, consumed for safety) --- */
		if (*format == 'l') { format++; if (*format == 'l') format++; }

		/* --- Conversion specifier --- */
		char tmp[32];
		int len;

		switch (*format) {
		case 'd':
		case 'i': {
			int val = va_arg(parameters, int);
			int neg = 0;
			unsigned int uval;
			if (val < 0) { neg = 1; uval = (unsigned int)(-(val + 1)) + 1u; }
			else          { uval = (unsigned int)val; }

			len = render_unsigned(tmp, sizeof(tmp), uval, 10, 0);

			/* Minimum digits from precision */
			int min_digits = (precision >= 0) ? precision : 1;
			int num_digits = len > min_digits ? len : min_digits;
			int total = num_digits + neg;

			/* When precision is set, width pad is always space */
			char wpad = (precision >= 0) ? ' ' : pad;

			if (!left_align)
				for (int p = total; p < width; p++)
					{ if (!print_char(wpad)) return -1; written++; }
			if (neg)
				{ if (!print_char('-')) return -1; written++; }
			for (int p = len; p < min_digits; p++)
				{ if (!print_char('0')) return -1; written++; }
			for (int p = len - 1; p >= 0; p--)
				{ if (!print_char(tmp[p])) return -1; written++; }
			if (left_align)
				for (int p = total; p < width; p++)
					{ if (!print_char(' ')) return -1; written++; }
			break;
		}
		case 'u': {
			unsigned int val = va_arg(parameters, unsigned int);
			len = render_unsigned(tmp, sizeof(tmp), val, 10, 0);

			int min_digits = (precision >= 0) ? precision : 1;
			int num_digits = len > min_digits ? len : min_digits;
			char wpad = (precision >= 0) ? ' ' : pad;

			if (!left_align)
				for (int p = num_digits; p < width; p++)
					{ if (!print_char(wpad)) return -1; written++; }
			for (int p = len; p < min_digits; p++)
				{ if (!print_char('0')) return -1; written++; }
			for (int p = len - 1; p >= 0; p--)
				{ if (!print_char(tmp[p])) return -1; written++; }
			if (left_align)
				for (int p = num_digits; p < width; p++)
					{ if (!print_char(' ')) return -1; written++; }
			break;
		}
		case 'x':
		case 'X': {
			unsigned int val = va_arg(parameters, unsigned int);
			int upper = (*format == 'X');
			len = render_unsigned(tmp, sizeof(tmp), val, 16, upper);

			int min_digits = (precision >= 0) ? precision : 1;
			int num_digits = len > min_digits ? len : min_digits;
			char wpad = (precision >= 0) ? ' ' : pad;

			if (!left_align)
				for (int p = num_digits; p < width; p++)
					{ if (!print_char(wpad)) return -1; written++; }
			for (int p = len; p < min_digits; p++)
				{ if (!print_char('0')) return -1; written++; }
			for (int p = len - 1; p >= 0; p--)
				{ if (!print_char(tmp[p])) return -1; written++; }
			if (left_align)
				for (int p = num_digits; p < width; p++)
					{ if (!print_char(' ')) return -1; written++; }
			break;
		}
		case 'p': {
			unsigned int val = (unsigned int)(uintptr_t)va_arg(parameters, void *);
			if (!print("0x", 2)) return -1;
			written += 2;
			len = render_unsigned(tmp, sizeof(tmp), val, 16, 0);
			for (int p = len; p < 8; p++)
				{ if (!print_char('0')) return -1; written++; }
			for (int p = len - 1; p >= 0; p--)
				{ if (!print_char(tmp[p])) return -1; written++; }
			break;
		}
		case 'c': {
			char c = (char) va_arg(parameters, int);
			if (!print_char(c)) return -1;
			written++;
			break;
		}
		case 's': {
			const char* str = va_arg(parameters, const char*);
			if (!str) str = "(null)";
			int slen = (int)strlen(str);
			if (precision >= 0 && slen > precision)
				slen = precision;
			if (!left_align)
				for (int p = slen; p < width; p++)
					{ if (!print_char(' ')) return -1; written++; }
			if (!print(str, slen)) return -1;
			written += slen;
			if (left_align)
				for (int p = slen; p < width; p++)
					{ if (!print_char(' ')) return -1; written++; }
			break;
		}
		case '%':
			if (!print_char('%')) return -1;
			written++;
			break;
		case '\0':
			goto done;
		default:
			/* Unknown specifier — print it literally */
			if (!print_char('%')) return -1;
			written++;
			if (!print_char(*format)) return -1;
			written++;
			break;
		}
		format++;
	}
done:
	va_end(parameters);
	return written;
}
