# MOSS
Hobbyist 16-bit real mode operating system written in assembly

Contains:

-Basic bootloader, loaded into memory location 0x7c00

-Basic kernel onto which bootloader loads and from which shell commands are executed

-Functional user input system, shell commands

-Bootable on any x86-compatible CPU

**How to compile & run:**

-Ensure you have bochs installed

-Navigate to bin directory

-Run 'make MOSS'

-Run bochs, then choose option 6 and type 'c'
