; MOSS bootloader
; Miro Haapalainen (Merplop), 2020

org 0x7c00		; load boot sector into memory location 0xc700

mov bx, 0x1000		; load into memory address 0x1000
mov es, bx
mov bx, 0x0		; set ES:BX to 0x1000:0x0

; configure disk for load
mov dh, 0x0		; set head to 0
mov dl, 0x0		; set drive to 0
mov ch, 0x0		; set cylinder to 0
mov cl, 0x02		; jumpto this starting sector for disk read

loaddisk:
mov ah, 0x02		; read disk sectors
mov al, 0x01		; parse number of sectors to read
int 0x13

jc loaddisk

mov ax, 0x1000
mov ds, ax
mov es, ax
mov fs, ax
mov gs, ax
mov ss, ax

jmp 0x1000:0x0

times 510-($-$$) db 0	; add to boot sector value of 510 minus bytes of written code in zeroes
dw 0xaa55		; 'magic number'
