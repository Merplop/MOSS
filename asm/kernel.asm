call mscreenr

mov si, welcome
   call print_string
 
 mainloop:
   mov si, prompt
   call print_string
 
   mov di, buffer
   call get_string
 
   mov si, buffer
   cmp byte [si], 0  ; blank line?
   je mainloop       ; yes, ignore it
 
   mov si, buffer
   mov di, reboot_cmd
   call strcmp
   jc .reboot
 
   mov si, buffer
   mov di, help_cmd
   call strcmp
   jc .help

   mov si, buffer
   mov di, ver_cmd
   call strcmp
   jc .ver

   mov si, buffer
   mov di, hex_cmd
   call strcmp
   jc .phex
 
   mov si,badcommand
   call print_string 
   jmp mainloop  
 
 .ver:
   mov si, ver_str
   call print_string
 
   jmp mainloop
 
 .help:
   mov si, help_str
   call print_string
 
   jmp mainloop

 .reboot:
   jmp 0xFFFF:0x0000

 .clear:
   jmp clear_screen

 .phex:
   jmp print_hex

 welcome: db 'MOSS (Merplop Open Source System) v0.04alpha', 0x0D, 0x0A, '--------------------------------------------', 0x0D, 0x0A, 'Type a command or run "help" for list of internal commands', 0x0D, 0x0A, 0
 badcommand: db 'ERROR 01 - Invalid command', 0x0D, 0x0A, 0
 prompt: db ':$', 0
 skipline: db 0xA, 0xD, 0

 ; internal commands

 reboot_cmd:	db 'reb', 0		; reboot kernel
 hcp_cmd:	    db 'hcp', 0		; halt cpu command
 help_cmd:  	db 'help', 0		; print internal commands to screen
 clr_cmd:	    db 'clr', 0		; clear screen
 ver_cmd:	    db 'ver', 0		; display kernel version
 ls_cmd:    	db 'ls', 0		; display contents of directory
 colour_cmd:    db 'colour', 0		; display usage information for 'colour' command
 hex_cmd:       db 'phex', 0
 buffer: times 64 db 0
 
 ; ================
 ; calls start here
 ; ================
 
 print_string:
   lodsb        ; grab a byte from SI
 
   or al, al  ; logical or AL by itself
   jz .done   ; if the result is zero, get out
 
   mov ah, 0x0E
   int 0x10      ; otherwise, print out the character!
 
   jmp print_string
 
 .done:
   ret
 
 get_string:
   xor cl, cl
 
 .loop:
   mov ah, 0
   int 0x16
 
   cmp al, 0x08
   je .backspace
 
   cmp al, 0x0D
   je .done

   cmp al, 0x0C
   je clear_screen
 
   cmp cl, 0x3F
   je .loop
 
   mov ah, 0x0E
   int 0x10
 
   stosb  ; put character in buffer
   inc cl
   jmp .loop
   
 .backspace:
   cmp cl, 0	; beginning of string?
   je .loop	; yes, ignore the key
 
   dec di
   mov byte [di], 0	; delete character
   dec cl		; decrement counter as well
 
   mov ah, 0x0E
   mov al, 0x08
   int 10h		; backspace on the screen
 
   mov al, ' '
   int 10h		; blank character out
 
   mov al, 0x08
   int 10h		; backspace again
 
   jmp .loop	; go to the main loop
 
 .done:
   mov al, 0	; null terminator
   stosb
 
   mov ah, 0x0E
   mov al, 0x0D
   int 0x10
   mov al, 0x0A
   int 0x10		; newline
 
   ret
 
 strcmp:
 .loop:
   mov al, [si]   ; grab a byte from SI
   mov bl, [di]   ; grab a byte from DI
   cmp al, bl     ; are they equal?
   jne .notequal  ; nope, we're done.
 
   cmp al, 0  ; are both bytes (they were equal before) null?
   je .done   ; yes, we're done.
 
   inc di     ; increment DI
   inc si     ; increment SI
   jmp .loop  ; loop!
 
 .notequal:
   clc  ; not equal, clear the carry flag
   ret
 
 .done: 	
   stc  ; equal, set the carry flag
   ret

clear_screen:
   mov ah, 0x00	; set video mode
   mov al, 0x03	; 80x25 text mode
   int 0x10	; sets up interrupt handler for video services

   mov ah, 0x0B	; set colour scheme
   mov bh, 0x00
   mov bl, 0x09
   int 0x10

   jmp mainloop

print_hex:
   mov [.temp],al
   shr al,4
   cmp al,10
   sbb al,69h
   das
 
   mov ah,0Eh
   int 10h
 
   mov al,[.temp]
   ror al,4
   shr al,4
   cmp al,10
   sbb al,69h
   das
 
   mov ah,0Eh
   int 10h
   mov si, skipline
   call print_string
 
   jmp mainloop
 
   .temp db 0
   

; includescanvas

 include "graph/mscreenr.asm"
 include "inc/help.asm"
 include "inc/ver.asm"
 
   times 8192-($-$$) db 0
   dw 0AA55h ; some BIOSes require this signature .clear_screen:
