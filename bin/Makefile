# Basic makefile to make my life less painful

# OS binary file
MOSS: bootsect.bin kernel.bin
	cat bootsect.bin kernel.bin > MOSS.bin
# Bootsector binary file
bootsect.bin:
	fasm ../asm/bootsect.asm
	mv ../asm/bootsect.bin ./
# Kernel binary file
kernel.bin:
	fasm ../asm/kernel.asm
	mv ../asm/kernel.bin ./

# Get rid of everything
clean:
	rm *.bin
