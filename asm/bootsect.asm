; MOSS bootloader
; Miro Haapalainen (Merplop), 2020-2021
; GNU GPL License v.3

org 0x7c00		; load boot sector into memory offset 0xc700

mov bx, 0x1000		; load into memory address 0x1000
mov es, bx
mov bx, 0x0		; set ES:BX to 0x1000:0x0

; configure hard disk for kernel dump
mov dh, 0x0		; set head to 0
mov dl, 0x0		; set drive to 0
mov ch, 0x0		; set cylinder to 0
mov cl, 0x05		; jumpto this starting sector for disk read

firststage:
mov ah, 0x02		; read disk sectors
mov al, 0x01		; parse number of sectors to read
int 0x13		; sets up real-mode interrupt handler for disk read/write functionality

jc firststage   ; if fail, loop back to firststage

mov bx, 0x2000
mov es, bx
mov bx, 0x0

mov dh, 0x0
mov dl, 0x0
mov ch, 0x0
mov cl, 0x02

secondstage:
mov ah, 0x02
mov al, 0x03
int 0x13

jc secondstage


mov ax, 0x2000
mov ds, ax
mov es, ax
mov fs, ax
mov gs, ax
mov ss, ax

jmp 0x2000:0x0		; jump to dumped kernel code

times 510-($-$$) db 0	; add to boot sector value of 510 minus bytes of written code in zeroes

dw 0xaa55		; magic number - cpu will not recognise file as bootsector without this
