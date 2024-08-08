bits 16

%define EL 0x0D, 0x0A

%define fat12 1
%define fat16 2
%define fat32 3
%define ext2 4

section .fsjump
	jmp short start
	nop

section .fsheaders

%if (FILESYSTEM == fat12) || (FILESYSTEM == fat16) || (FILESYSTEM == fat32)
	bdb_oem: db "abcdefgh"
	bdb_bytes_per_sector: dw 512
	bdb_sectors_per_cluster: db 1
	bdb_reserved_sectors: dw 1
	bdb_fat_count: db 2
	bdb_dir_entries_count: dw 0E0h
	bdb_total_sectors: dw 2880
	bdb_media_descriptor_type: db 0F0h
	bdb_sectors_per_fat: dw 9
	bdb_sectors_per_track: dw 18
	bdb_heads: dw 2
	bdb_hidden_sectors: dd 0
	bdb_large_sector_count: dd 0

	%if (FILESYSTEM == fat32)
		fat32_sectors_per_fat: dd 0
		fat32_flags: dw 0
		fat32_fat_version_number: dw 0
		fat32_rootdir_cluster: dd 0
		fat32_fsinfo_sector: dw 0
		fat32_backup_boot_sector: dw 0
		fat32_reserved: times 12 db 0
	%endif

	ebr_drive_number: db 0
	db 0
	ebr_signature: db 29h
	ebr_volume_id: db 12h, 34h, 56h, 78h
	ebr_volume_label: db 'MOSS'
	ebr_system_id: db 'FAT12   '

%endif

section .entry
	global start

	start:
		mov ax, PARTITION_ENTRY_SEGMENT
		mov es, ax
		mov di, PARTITION_ENTRY_OFFSET
		mov cx, 16
		rep movsb

		xor ax, ax
		mov ds, ax
		mov es, ax

		mov ss, ax
		mov sp, 0x7C00

		push es
		push word .after
		retf
	.after:

		mov [ebr_drive_number], dl
		
		mov ah, 0x41
		mov bx, 0x55AA
		stc
		int 13h
		
		jc .no_disk_extensions

		mov byte [have_extensions], 1
		jmp .after_disk_extensions_check

	.no_disk_extensions:
		mov byte [have_extensions], 0

	.after_disk_extensions_check:
		mov si, stage2_location

		mov ax, STAGE2_LOAD_SEGMENT
		mov es, ax

		mov bx, STAGE2_LOAD_OFFSET

	.loop:
		mov eax, [si]
		add si, 4
		mov cl, [si]
		inc si

		cmp eax, 0
		je .read_finish

		call disk_read

		xor ch, ch
		shl cx, 5
		mov di, es
		add di, cx
		mov es, di

		jmp .loop

	.read_finish:

		mov dl, [ebr_drive_number]
		mov si, PARTITION_ENTRY_OFFSET
		mov di, PARTITION_ENTRY_SEGMENT

		mov ax, STAGE2_LOAD_SEGMENT
		mov ds, ax
		mov es, ax

		jmp STAGE2_LOAD_SEGMENT:STAGE2_LOAD_OFFSET

		jmp wait_key_and_reboot

		cli
		hlt

section .text

	floppy_error:
		mov si, msg_read_failed
		call puts
		jmp wait_key_and_reboot

	kernel_not_found_error:
		mov si, msg_stage2_not_found
		call puts
		jmp wait_key_and_reboot

	wait_key_and_reboot:
		mov ah, 0
		int 16h
		jmp 0FFFFh:0

	.halt:
		cli
		hlt
