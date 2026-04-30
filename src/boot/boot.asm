; boot.asm
[org 0x7C00]
bits 16

LOADER2_SEGMENT  equ 0x4000
LOADER2_OFFSET   equ 0x0000
BOOT_SECTOR_SIZE equ 512
LOADER2_SECTORS_PATCH_OFFSET equ 488
FAT16_LBA_PATCH_OFFSET equ 504

start:
    mov [BOOT_DRIVE], dl

    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    ; Stage 1 only needs a temporary stack while it loads loader2.
    mov sp, 0x9000
    sti

    mov si, boot_msg
    call print_string

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
    jmp .print_loop
.done:
    popa
    ret

load_loader2:
    pusha

    mov ax, LOADER2_SEGMENT
    mov es, ax
    xor bx, bx

    mov dl, [BOOT_DRIVE]
    xor ax, ax
    int 0x13
    jc disk_error

    mov al, [loader2_sectors]
    mov ah, 0x02
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

times LOADER2_SECTORS_PATCH_OFFSET-($-$$) db 0
loader2_sectors dd 0
times FAT16_LBA_PATCH_OFFSET-($-$$) db 0
fat16_start_lba dd 0

times BOOT_SECTOR_SIZE-BOOT_SIGNATURE_SIZE-($-$$) db 0
dw 0xAA55
