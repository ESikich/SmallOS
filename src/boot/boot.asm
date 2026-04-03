; boot.asm
[org 0x7C00]
bits 16

LOADER2_OFFSET   equ 0xA000
BOOT_SECTOR_SIZE equ 512
LOADER2_SECTORS  equ 4

start:
    mov [BOOT_DRIVE], dl

    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x9000
    sti

    mov si, boot_msg
    call print_string

    call load_loader2

    mov si, loaded_msg
    call print_string

    mov dl, [BOOT_DRIVE]
    jmp 0x0000:LOADER2_OFFSET

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
    jmp .print_loop
.done:
    popa
    ret

load_loader2:
    pusha

    xor ax, ax
    mov es, ax
    mov bx, LOADER2_OFFSET

    mov dl, [BOOT_DRIVE]
    xor ax, ax
    int 0x13
    jc disk_error

    mov ah, 0x02
    mov al, LOADER2_SECTORS
    mov ch, 0
    mov cl, 2
    mov dh, 0
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc disk_error

    popa
    ret

disk_error:
    mov si, disk_msg
    call print_string
    jmp $

BOOT_DRIVE db 0
boot_msg   db "Booting stage2...", 0
loaded_msg db " loaded", 0
disk_msg   db " Disk read error!", 0

BOOT_SIGNATURE_SIZE    equ 2
FAT16_LBA_PATCH_SIZE   equ 4
FAT16_LBA_PATCH_OFFSET equ 504

times FAT16_LBA_PATCH_OFFSET-($-$$) db 0
fat16_start_lba dd 0
times BOOT_SECTOR_SIZE-BOOT_SIGNATURE_SIZE-($-$$) db 0
dw 0xAA55