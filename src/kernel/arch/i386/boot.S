# Declare constants for the multiboot header.
.set ALIGN,    1<<0             # align loaded modules on page boundaries
.set MEMINFO,  1<<1             # provide memory map
.set VIDMODE,  1<<2             # request video mode from bootloader
.set FLAGS,    ALIGN | MEMINFO | VIDMODE
.set MAGIC,    0x1BADB002       # 'magic number' lets bootloader find the header
.set CHECKSUM, -(MAGIC + FLAGS) # checksum of above, to prove we are multiboot

# Declare a header as in the Multiboot Standard.
.section .multiboot
.align 4
.long MAGIC
.long FLAGS
.long CHECKSUM
# Address fields (offsets 12-28, must be present for video fields at offset 32)
.long 0              # header_addr   (unused, no AOUT_KLUDGE)
.long 0              # load_addr
.long 0              # load_end_addr
.long 0              # bss_end_addr
.long 0              # entry_addr
# Video mode request (offset 32-44)
.long 0              # mode_type: 0 = linear graphics framebuffer
.long 1920           # width  (pixels)
.long 1080           # height (pixels)
.long 32             # depth  (bits per pixel)

# Reserve a stack for the initial thread.
.section .bss
.align 16
stack_bottom:
.skip 16384 # 16 KiB
stack_top:

# ----- GDT -----
# GRUB's GDT selectors are not guaranteed; set up our own so that
# 0x08 = flat 32-bit code and 0x10 = flat 32-bit data (ring 0).
.section .data
.align 8
gdt_start:
    # Null descriptor
    .long 0, 0
    # 0x08: Code segment – base 0, limit 4 GiB, 32-bit, ring 0
    .word 0xFFFF        # limit low
    .word 0x0000        # base low
    .byte 0x00          # base mid
    .byte 0x9A          # access: present, ring 0, code, exec/read
    .byte 0xCF          # granularity: 4K pages, 32-bit, limit high 0xF
    .byte 0x00          # base high
    # 0x10: Data segment – base 0, limit 4 GiB, 32-bit, ring 0
    .word 0xFFFF
    .word 0x0000
    .byte 0x00
    .byte 0x92          # access: present, ring 0, data, read/write
    .byte 0xCF
    .byte 0x00
gdt_end:

gdt_descriptor:
    .word gdt_end - gdt_start - 1   # limit
    .long gdt_start                  # base

# The kernel entry point.
.section .text
.global _start
.type _start, @function
_start:
	movl $stack_top, %esp

	# Save multiboot registers before they are clobbered.
	movl %eax, %edi        # magic
	movl %ebx, %esi        # multiboot info pointer

	# Load our own GDT so selectors 0x08/0x10 are well-defined.
	lgdt gdt_descriptor
	# Reload CS via a far jump.
	ljmp $0x08, $.reload_segments
.reload_segments:
	movw $0x10, %ax
	movw %ax, %ds
	movw %ax, %es
	movw %ax, %fs
	movw %ax, %gs
	movw %ax, %ss

	# Restore multiboot arguments for _main(mbd, magic).
	pushl %edi             # magic  (second argument)
	pushl %esi             # mbd    (first argument)

	# Call the global constructors.
	call _init

	# Transfer control to the main kernel.
	call _main

	# Hang if kernel_main unexpectedly returns.
	cli
1:	hlt
	jmp 1b
.size _start, . - _start
