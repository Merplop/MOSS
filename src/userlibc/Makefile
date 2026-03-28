# MOSS User-Space C Library
# Builds libc.a and crt0.o for user programs.

CC = i686-elf-gcc
AR = i686-elf-ar
AS = i686-elf-gcc

CFLAGS = -O2 -g -ffreestanding -nostdlib -Wall -Wextra -Iinclude -std=gnu11

SRCS = \
	src/unistd.c \
	src/stdio.c \
	src/stdlib.c \
	src/string.c \
	src/errno.c \
	src/fb.c \
	src/keyboard.c \
	src/math.c \
	src/time.c \
	src/stat.c

OBJS = $(SRCS:.c=.o)
ASM_OBJS = setjmp.o

all: libc.a crt0.o

libc.a: $(OBJS) $(ASM_OBJS)
	$(AR) rcs $@ $(OBJS) $(ASM_OBJS)

crt0.o: crt0.S
	$(AS) -c $< -o $@

%.o: %.c
	$(CC) $(CFLAGS) -c $< -o $@

setjmp.o: setjmp.S
	$(AS) -c $< -o $@

clean:
	rm -f libc.a crt0.o setjmp.o $(OBJS)

.PHONY: all clean
