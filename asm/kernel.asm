; MOSS kernel
; Miro Haapalainen (Merplop), 2020-2021
; GNU GPL License v.3

kinit:
	call mscreenr

	; print heading and info msg
	mov si, .os_str
	call print

	mov si, .underscore_str
	call print

	mov si, .init_str
	call print

getinput:
	mov si, prompt		; prints prompt to screen
	call print		; calls print function
	xor cx, cx		; clear cx register for looping through user input functions
	mov si, cmdstr

	mov ax, 0x2000
	mov es, ax
	mov ds, ax
		
keyloop:
	xor ax, ax		; clear out accumulator
	int 16h			; key stroke interrupt

	mov ah, 0x0e
	cmp al, 0xD		; checks for "enter" key press
	je execcmd

	int 0x10
	mov [si], al
	inc cx
	inc si
	jmp keyloop		; loop back to start of function

execcmd:
	cmp cx, 0		; if loop count is zero, print cmd fail msg to screen
	je cmdf

	mov byte [si], 0 
	mov si, cmdstr

chcmd:
	push cx
	mov di, reboot_cmd
	repe cmpsb
	je reboot

	pop cx
	push cx
	mov di, hcp_cmd
	mov si, cmdstr
	repe cmpsb
	je halt_cpu

	pop cx
	push cx
	mov di, help_cmd
	mov si, cmdstr
	repe cmpsb
	je help_msg

	pop cx
	push cx
	mov di, ver_cmd
	mov si, cmdstr
	repe cmpsb
	je ver_msg

	pop cx
	push cx
	mov di, clr_cmd
	mov si, cmdstr
	repe cmpsb
	je clear_cmd

	pop cx
	push cx
	mov di, colour0_cmd
	mov si, cmdstr
	repe cmpsb
	je colour0

	pop cx
	push cx
	mov di, colour1_cmd
	mov si, cmdstr
	repe cmpsb
	je colour1

	pop cx
	push cx
	mov di, colour2_cmd
	mov si, cmdstr
	repe cmpsb
	je colour2

	pop cx
	push cx
	mov di, colour3_cmd
	mov si, cmdstr
	repe cmpsb
	je colour3

	pop cx
	push cx
	mov di, colour4_cmd
	mov si, cmdstr
	repe cmpsb
	je colour4

	pop cx
	push cx
	mov di, colour5_cmd
	mov si, cmdstr
	repe cmpsb
	je colour5

	pop cx
	push cx
	mov di, colour6_cmd
	mov si, cmdstr
	repe cmpsb
	je colour6

	pop cx
	push cx
	mov di, colour7_cmd
	mov si, cmdstr
	repe cmpsb
	je colour7

	pop cx
	push cx
	mov di, colour8_cmd
	mov si, cmdstr
	repe cmpsb
	je colour8

	pop cx
	push cx
	mov di, colour9_cmd
	mov si, cmdstr
	repe cmpsb
	je colour9

	pop cx

cmdf:
	mov si, cmd_fail	; prints failure msg
	call print
	jmp getinput		; return to get input mode

halt_cpu:
	mov ah, 0x00
	mov al, 0x03
	int 0x10
	popa
	cli
	hlt





; internal commands

reboot_cmd:	db 'reb', 0		; reboot kernel
hcp_cmd:	db 'hcp', 0		; halt cpu command
help_cmd:	db 'help', 0		; print internal commands to screen
clr_cmd:	db 'clr', 0		; clear screen
ver_cmd:	db 'ver', 0		; display kernel version

; colour commands
colour0_cmd:	db 'colour 0', 0		; change background colour to 0x00 
colour1_cmd:	db 'colour 1', 0		; change background colour to 0x01
colour2_cmd:	db 'colour 2', 0		; change background colour to 0x02
colour3_cmd:	db 'colour 3', 0		; change background colour to 0x03
colour4_cmd:	db 'colour 4', 0		; change background colour to 0x04
colour5_cmd:	db 'colour 5', 0		; change background colour to 0x05
colour6_cmd:	db 'colour 6', 0		; change background colour to 0x06
colour7_cmd:	db 'colour 7', 0		; change background colour to 0x07
colour8_cmd:	db 'colour 8', 0		; change background colour to 0x08
colour9_cmd:	db 'colour 9', 0		; change background colour to 0x09

; includes

include "graph/mscreenr.asm"
include "inc/help.asm"
include "inc/ver.asm"
include "graph/print.asm"
include "inc/initstr.asm"
include "inc/reboot.asm"
include "graph/clr.asm"

; colour command includes

include "inc/colour/colour0.asm"
include "inc/colour/colour1.asm"
include "inc/colour/colour2.asm"
include "inc/colour/colour3.asm"
include "inc/colour/colour4.asm"
include "inc/colour/colour5.asm"
include "inc/colour/colour6.asm"
include "inc/colour/colour7.asm"
include "inc/colour/colour8.asm"
include "inc/colour/colour9.asm"

cmd_fail: db 0xA, 0xD, 'ERROR 01: Invalid command or filename', 0xA, 0xD, 0		; for byte definitions, 0xA is line feed, 0xD is carriage return, 0 is end

prompt: db '$:', 0

skipline: db 0xA, 0xD, 0

colour_warning: db 'WARN - some BIOS colours may be difficult to read', 0xA, 0xD, 0

cmdstr: db ''

times 2048-($-$$) db 0
