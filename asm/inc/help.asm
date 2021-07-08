; print help message onto screen

help_msg:
	mov si, .help_str
	call print
	jmp getinput

.help_str:

db 0xA, 0xD, 'MOSS Kernel Internal Commands', 0xA, 0xD
db 	'reb - Reboot kernel', 0xA, 0xD
db	'help - Display this message', 0xA, 0xD
db	'hcp - Halt CPU', 0xA, 0xD
db	'ver - Display kernel version and development information', 0xA, 0xD
db	'clr - Clear screen, reinitalise screeninit service', 0xA, 0xD
db	'colour <number> - Change background colour', 0xA, 0xD, 0
