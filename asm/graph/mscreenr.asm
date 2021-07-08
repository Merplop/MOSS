; MOSS Screen Refresher

mscreenr:
  mov ah, 0x00	    ; set video mode
  mov al, 0x03	    ; 80x25 text mode
  int 0x10	        ; sets up interrupt handler for video services

  mov ah, 0x0B    	; set colour scheme
  mov bh, 0x00
  mov bl, 0x09      ; light blue default colour
  int 0x10

  ret
