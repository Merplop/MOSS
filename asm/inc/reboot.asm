; reboot (reb) command

reboot:
	jmp 0xFFFF:0x0000	; jump to reset vector
