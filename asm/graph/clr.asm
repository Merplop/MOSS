; clear screen command
; now carries background colour over

clear_cmd:

mov ah, 0x00
mov al, 0x03
int 0x10

mov ah, 0x0B
mov bh, 0x00
int 0x10

jmp getinput
