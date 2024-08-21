# MOSS (Miro's Open Source System); latest release: v1.1beta

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

-Run the command 'sudo dd if=myos.iso of=/dev/sdx && sync'

-Ensure you are booting onto the drive in EFI/legacy mode, as UEFI mode doesn't support VGA text mode correctly.

# Note about Assembly version

I have decided to keep the source code for the old x86-Assembly kernel in this repo, but that version is officially deprecated.
If you still wish to run it, however, you can navigate to the bin directory, run make MOSS, run bochs, hit '6', and hit 'c'. The
functionality of this version is, however, severely limited and was written when I was in high school.
