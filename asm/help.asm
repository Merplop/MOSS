helpcmd:

mov si, .helpm
push 0x02
int 0x80

jmp getinput

.helpm:

db "MOSS internal commands", 0x0A
db 0x0A
db "cd     -     change directory", 0x0A
db "tex    -     basic text editor", 0x0A
db "dst    -     display system time", 0x0A
db "ls     -     show contents of directory", 0x0A
db "end    -     halts processor", 0x0A
db "ver    -     displays version and development information", 0x0A
db 0x00
