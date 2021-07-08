; string print utility
; prints contents of SI register onto screen

print:
	pusha			; push all registers
	xor ax, ax		; clear AX
	xor bx, bx		; clear BX
	mov ah, 0x0e
	mov bh, 0x00
	mov bl, 0x07

pchar:
	lodsb			; load byte at address DS:(E)SI into AL
	cmp al, 0		; compare AL to 0
	je endprint		; if true, end print cycle
	int 0x10		; interrupt for video services
	jmp pchar		; jump back to loop until AL = 0

endprint:
	popa			; pop all registers
	ret			; return
