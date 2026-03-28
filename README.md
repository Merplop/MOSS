# MOSS (Miro's Open Source System); latest release: 2.0beta

Release notes: v2.0beta

-Virtual memory and Paging

-ISR/IDT

-1080p resolution

-Round-robin scheduling

Starting to look like an actual, usable kernel.

Release notes: v1.1beta

-Kernel now reads memory map from GRUB bootloader

-Beginnings of physical memory manager now implemented, initialising non-reserved memory regions as usable on startup


Release Notes: v1.0beta

The kernel is now officially translated to C and runs in 32-bit mode.

Features:

-Functional keyboard and video driver + functional shell

-MFS, a rudimentary 'file system' which saves file and directory data to memory (FAT32 file system currently in progress)

-tex, a basic text editor for editting MOSS-formatted data files

Current features being developed:

-FAT32 driver

-Process execution handler

-Physical memory manager

-Multithreading

# To run (QEMU; requires Arch Linux-based distribution):

-Ensure you have QEMU installed

-Install the i686-elf toolchain (i686-elf-gcc package on the AUR)

-Navigate to the C directory and run the shell script ./run; the cross-compiler will automatically compile the OS and QEMU will run it

# To run (real hardware; requires Unix system):

-Run lsblk and choose which drive to burn it onto; I recommend a USB flash drive since this will wipe everything on your drive of choice

-Run the command 'sudo dd if=moss.iso of=/dev/sdx && sync'

-Ensure you are booting onto the drive in EFI/legacy mode, as UEFI mode doesn't support VGA text mode correctly.
