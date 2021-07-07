print:
	pusha
;	xor ax, ax
;	xor bx, bx
	mov ah, 0x0e
	mov bh, 0x00
	mov bl, 0x07

pchar:
	lodsb
	cmp al, 0
	je endprint
	int 0x10
	jmp pchar

endprint:
	popa
	ret
