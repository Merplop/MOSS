#
# Makefile for MOSS user-space programs (shell + utilities)
#
# Usage:
#   . ../config.sh (or set CC, etc.)
#   make
#
# All programs are statically linked against userlibc.
#

CC      = i686-elf-gcc
CFLAGS  = -O2 -g -ffreestanding -nostdlib -Wall -Wextra -I../userlibc/include -std=gnu11
LDFLAGS = -ffreestanding -nostdlib -L../userlibc -T../userlibc/user.ld
LIBS    = ../userlibc/crt0.o -lc -lgcc

PROGS = msh ls cat echo mkdir rm cp wc head grep

all: $(PROGS)

msh: ../msh.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

ls: ls.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

cat: cat.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

echo: echo.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

mkdir: mkdir.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

rm: rm.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

cp: cp.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

wc: wc.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

head: head.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

grep: grep.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $@ $< $(LIBS)

clean:
	rm -f $(PROGS)

.PHONY: all clean
