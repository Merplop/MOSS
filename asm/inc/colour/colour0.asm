colour0:
	mov ah, 0x0B
	mov bh, 0x00
	mov bl, 0x00
	int 0x10
	mov si, skipline
	call print
	mov si, colour_warning
	call print
	jmp getinput
