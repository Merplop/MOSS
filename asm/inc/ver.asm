ver_msg:
	mov si, .ver_str
	call print
	jmp getinput

.ver_str:

db 0xA, 0xD, 'MOSS Kernel Version 0.04 alpha', 0xA, 0xD
db	'2020-2021, Miro Haapalainen', 0xA, 0xD
db	'Licensed under GNU GPL License v.3', 0xA, 0xD, 0
