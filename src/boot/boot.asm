; boot.asm
[org 0x7C00]
bits 16

LOADER2_SEGMENT  equ 0x4000
LOADER2_OFFSET   equ 0x0000
LOADER2_SECTORS  equ 16
BOOT_SECTOR_SIZE equ 512
MBR_PARTITION_TABLE_OFFSET equ 446
MBR_PARTITION_ENTRY_SIZE   equ 16

start:
    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    ; Stage 1 only needs a temporary stack while it loads loader2.
    mov sp, 0x9000
    sti

    mov [BOOT_DRIVE], dl

    call draw_stage1_ui

    call load_loader2

    mov si, loaded_msg
    call print_string

    mov dl, [BOOT_DRIVE]
    jmp LOADER2_SEGMENT:LOADER2_OFFSET

hang:
    jmp hang

print_string:
    pusha
.print_loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    cmp al, 10
    jne .print_loop
    call set_left_margin
    jmp .print_loop
.done:
    popa
    ret

set_left_margin:
    pusha
    mov ah, 0x03
    mov bh, 0
    int 0x10
    mov ah, 0x02
    mov bh, 0
    mov dl, 1
    int 0x10
    popa
    ret

draw_stage1_ui:
    pusha
    mov ax, 0x0003
    int 0x10
    mov ax, 0x0600
    mov bh, 0x07
    xor cx, cx
    mov dx, 0x184F
    int 0x10

    mov ah, 0x02
    mov bh, 0
    mov dx, 0x0101
    int 0x10

    mov si, stage1_msg
    call print_string
    popa
    ret

load_loader2:
    pusha

    mov ax, LOADER2_SEGMENT
    mov [READ_SEGMENT], ax
    mov word [READ_OFFSET], LOADER2_OFFSET
    mov byte [READ_SECTOR], 2
    mov byte [READ_REMAINING], LOADER2_SECTORS

.read_sector:
    mov ax, [READ_SEGMENT]
    mov es, ax
    mov bx, [READ_OFFSET]
    mov ah, 0x02
    mov al, 1
    mov ch, 0
    mov cl, [READ_SECTOR]
    mov dh, 0
    mov dl, [BOOT_DRIVE]
    int 0x13
    pushf
    push ax
    xor ax, ax
    mov ds, ax
    pop ax
    popf
    jnc .read_ok
    mov [DISK_ERROR_CODE], ah
    jmp disk_error
.read_ok:
    add word [READ_OFFSET], BOOT_SECTOR_SIZE
    inc byte [READ_SECTOR]
    dec byte [READ_REMAINING]
    jnz .read_sector

    popa
    ret

disk_error:
    mov si, disk_msg
    call print_string
    mov si, drive_msg
    call print_string
    mov al, [BOOT_DRIVE]
    call print_hex8
    mov si, error_msg
    call print_string
    mov al, [DISK_ERROR_CODE]
    call print_hex8
    jmp $

print_hex8:
    pusha
    mov bl, al
    mov al, ' '
    call print_char
    mov al, '0'
    call print_char
    mov al, 'x'
    call print_char
    mov al, bl
    shr al, 4
    call print_hex_nibble
    mov al, bl
    and al, 0x0F
    call print_hex_nibble
    popa
    ret

print_hex_nibble:
    cmp al, 10
    jb .digit
    add al, 'A' - 10
    jmp print_char
.digit:
    add al, '0'
    jmp print_char

print_char:
    mov ah, 0x0E
    int 0x10
    ret

BOOT_DRIVE db 0
DISK_ERROR_CODE db 0
READ_SEGMENT dw 0
READ_OFFSET dw 0
READ_SECTOR db 0
READ_REMAINING db 0
stage1_msg db "SmallOS", 13, 10
           db "stage-1 bootstrap", 13, 10
           db 13, 10
           db "reading stage 2...", 13, 10, 0
loaded_msg db "stage 2 ready", 13, 10, 0
disk_msg   db " Disk read error!", 0
drive_msg  db " drive", 0
error_msg  db " err", 0

BOOT_SIGNATURE_SIZE    equ 2
times MBR_PARTITION_TABLE_OFFSET-($-$$) db 0

times BOOT_SECTOR_SIZE-BOOT_SIGNATURE_SIZE-($-$$) db 0
dw 0xAA55
