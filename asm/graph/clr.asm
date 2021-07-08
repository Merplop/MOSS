; clear screen command
; for now, just calls mscreenr. Will have to develop a proper implementation later

clear_cmd:
call mscreenr
jmp getinput
