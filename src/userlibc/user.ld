/*
 * user.ld — Linker script for MOSS user-space programs.
 *
 * User programs are loaded at virtual address 0x08048000
 * (the traditional Linux user-space base).
 */
ENTRY(_start)

SECTIONS
{
    . = 0x08048000;

    .text BLOCK(4K) : ALIGN(4K)
    {
        *(.text .text.*)
    }

    .rodata BLOCK(4K) : ALIGN(4K)
    {
        *(.rodata .rodata.*)
    }

    .data BLOCK(4K) : ALIGN(4K)
    {
        *(.data .data.*)
    }

    .bss BLOCK(4K) : ALIGN(4K)
    {
        *(.bss .bss.*)
        *(COMMON)
    }

    _end = .;
}
