; loader2.asm
;
; Stage 2 loader contract:
;   - loaded by boot sector to physical 0xA000
;   - occupies exactly 4 sectors (2048 bytes)
;   - kernel image begins at disk LBA KERNEL_LBA (0-based)
;   - kernel is loaded to physical 0x1000
;   - after loading, stage 2 enters 32-bit protected mode
;   - then jumps to kernel entry at 0x1000
;
; Safe kernel size:
;   Loader2 is at 0xA000.  Kernel loads to 0x1000.
;   Maximum safe kernel size before overwriting loader2:
;     (0xA000 - 0x1000) / 512 = 72 sectors = 36 KB
;   If the kernel exceeds this, move loader2 to 0xB000.

[org 0xA000]
bits 16

KERNEL_OFFSET        equ 0x1000
KERNEL_LBA           equ 5
KERNEL_SECTORS       equ __KERNEL_SECTORS__

start:
    mov [BOOT_DRIVE], dl

    cli
    xor ax, ax
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, 0x9000
    sti

    ; --- Check INT 0x13 extensions ---
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc .no_ext

    mov si, ext_ok_msg
    call print_string
    jmp .load

.no_ext:
    mov si, no_ext_msg
    call print_string
    jmp $               ; halt — cannot proceed without LBA support

.load:
    mov si, loader_msg
    call print_string

    call load_kernel

    mov si, kernel_loaded_msg
    call print_string

    call switch_to_pm

hang:
    jmp hang

; ---------------------------
print_string:
    pusha
.loop:
    lodsb
    test al, al
    jz .done
    mov ah, 0x0E
    int 0x10
    jmp .loop
.done:
    popa
    ret

; ---------------------------
lba_read:
    pusha
    mov ah, 0x42
    mov dl, [BOOT_DRIVE]
    mov si, dap
    int 0x13
    jc disk_error
    popa
    ret

load_kernel:
    pusha
    mov word  [dap_count], KERNEL_SECTORS
    mov word  [dap_off],   KERNEL_OFFSET
    mov word  [dap_seg],   0x0000
    mov dword [dap_lba],   KERNEL_LBA
    mov dword [dap_lba+4], 0
    call lba_read
    popa
    ret

disk_error:
    mov si, disk_msg
    call print_string
    jmp $

; ---------------------------
; Disk Address Packet
dap:
    db 0x10
    db 0x00
dap_count:
    dw 0
dap_off:
    dw 0
dap_seg:
    dw 0
dap_lba:
    dd 0
    dd 0

; ---------------------------
; Temporary GDT
gdt_start:
gdt_null:
    dq 0x0000000000000000
gdt_code:
    dq 0x00CF9A000000FFFF
gdt_data:
    dq 0x00CF92000000FFFF
gdt_end:

gdt_descriptor:
    dw gdt_end - gdt_start - 1
    dd gdt_start

CODE_SEG equ gdt_code - gdt_start
DATA_SEG equ gdt_data - gdt_start

switch_to_pm:
    cli
    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax
    jmp CODE_SEG:init_pm

[bits 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ebp, 0x90000
    mov esp, ebp
    mov eax, KERNEL_OFFSET
    jmp eax

.pm_hang:
    cli
    hlt
    jmp .pm_hang

[bits 16]
BOOT_DRIVE           db 0
ext_ok_msg           db "LBA ok ", 0
no_ext_msg           db "NO LBA!", 0
loader_msg           db "Loading...", 0
kernel_loaded_msg    db "K", 13, 10, 0
disk_msg             db "Disk err!", 0

LOADER2_SIZE_BYTES equ 2048
times LOADER2_SIZE_BYTES-($-$$) db 0