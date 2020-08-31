; MOSS kernel
; Miro Haapalainen (Merplop), 2020

mov ah, 0x00		; set video mode
mov al, 0x03		; 80x25
int 0x10

mov ah, 0x0B		; set colour scheme
mov bh, 0x00
mov bl, 0x09
int 0x10

; print heading and info message
mov si, string_1
call print

getinput:
mov si, prompt
call print

mov di, cmdstr

keyloop:
mov ax, 0x00
int 0x16		; key stroke int

mov ah, 0x0e
cmp al, 0xD		; checks for enter key press
je runcmd
int 0x10
mov [di], al
inc di
jmp keyloop

runcmd:
mov byte [di], 0
mov al, [cmdstr]
cmp al, 'h'
je cmdfnd
cmp al, 'n'
je end_programme
mov si, cmd_fail
call print
jmp getinput

cmdfnd:
mov si, cmd_success	; prints command success msg
call print
jmp getinput		; return to get input mode


print:
mov ah, 0x0e		; BIOS teletype output interrupt
mov bh, 0x0		; page #
mov bl, 0x07		; colour (white)

pchar:
mov al, [si]
cmp al, 0		; compare alow register to 0
je endprint		; jump to halt if equal to alow = 0
int 0x10		; print character in al
add si, 1		; get next char
jmp pchar		; initiate loop

endprint:
ret


end_programme:
cli
hlt


string_1: db 'MOSS (Merplop Open Source System) v0.01alpha', 0xA, 0xD, '-----------------------------------', 0xA, 0xD, 0xA, 0xD, 'Type a command or run "help" to get started', 0xA, 0xD, 0

cmd_success: db 0xA, 0xD, 'Command ran successfully', 0xA, 0xD, 0
cmd_fail: db 0xA, 0xD, 'ERROR 01: Invalid command or filename', 0xA, 0xD, 0
cmdstr: db ''

prompt: db '$', 0

times 1536-($-$$) db 0

; %include 'help.asm'
