# The master boot record the Tab5 puts on a new C.IMG (firmware/tab5_386/fd_install.h): it starts the active
# partition's boot sector, and while there is nothing to start (a partition not formatted: no 55 AA at its end; or a
# FAT12/16 one with no files in its root directory, as an installer leaves it when it restarts the PC after
# formatting), INT 18h, for the BIOS to boot the next device (the installer's CD). Our own; FreeDOS's installer
# writes its own over it.
#
# Build: tools/mbr/build.sh (GNU as), which makes firmware/tab5_386/mbr_code.h.
	.intel_syntax noprefix
	.code16
	.text
	.globl _start
_start:                         # at 0000:7C00, from the BIOS (DL: the drive)
	cli
	xor ax, ax
	mov ss, ax
	mov sp, 0x7c00
	mov ds, ax
	mov es, ax
	sti
	cld
	mov si, 0x7c00              # out of the way of the boot sector to load: to 0000:0600, where it is linked
	mov di, 0x0600
	mov cx, 256
	rep movsw
	.byte 0xea                  # jmp 0000:moved
	.word moved, 0
moved:
	mov [drive], dl
	mov si, offset parts
	mov cx, 4
1:	cmp byte ptr [si], 0x80     # the active partition
	je found
	add si, 16
	loop 1b
	jmp fail
found:
	mov bp, si                  # (DS:SI: its entry, for the boot sector, as other MBRs leave it)
	mov eax, [si + 8]           # its first sector (LBA), by INT 13h AH=42h
	mov [dap_lba], eax
	mov si, offset dap
	mov dl, [drive]
	mov ah, 0x42
	int 0x13
	jc fail
	cmp word ptr [0x7dfe], 0xaa55
	jne fail
	movzx ecx, word ptr [0x7c16]  # sectors per FAT (0: FAT32, not looked into)
	jecxz boot
	movzx eax, byte ptr [0x7c10]  # the root directory: after the reserved sectors and the FATs
	imul eax, ecx
	movzx ecx, word ptr [0x7c0e]
	add eax, ecx
	add [dap_lba], eax
	mov word ptr [dap_to], 0x7e00 # its first sector, after the boot sector
	mov si, offset dap
	mov dl, [drive]
	mov ah, 0x42
	int 0x13
	jc fail
	mov si, 0x7e00
	mov cx, 16
2:	cmp byte ptr [si], 0          # the end of the directory: no files
	je fail
	cmp byte ptr [si], 0xe5       # (deleted)
	je 3f
	test byte ptr [si + 11], 0x18 # a file (not a folder, the volume label, a long name's part)
	jz boot
3:	add si, 32
	loop 2b
boot:
	mov si, bp
	mov dl, [drive]
	.byte 0xea                  # jmp 0000:7C00
	.word 0x7c00, 0
fail:
	int 0x18                    # the next boot device
	jmp fail
dap:	.byte 16, 0
	.word 1                     # one sector
dap_to:	.word 0x7c00, 0             # to 0000:7C00
dap_lba: .long 0, 0
drive:	.byte 0
	.org 446
parts:	.space 64                # (the partition table, filled in by the firmware)
	.byte 0x55, 0xaa
