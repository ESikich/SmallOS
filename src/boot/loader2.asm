; loader2.asm
;
; Stage 2 loader contract:
;   - loaded by boot sector to physical 0x20000
;   - occupies exactly 4 sectors (2048 bytes)
;   - kernel image begins immediately before the FAT16 partition
;   - kernel is loaded to physical 0x1000
;   - after loading, stage 2 enters 32-bit protected mode
;   - then jumps to kernel entry at 0x1000
;
; Safe kernel size:
;   The build guard compares the kernel load span against both this loader's
;   physical address and the generated stage-2 stack top.
;   If the kernel exceeds that ceiling, the build fails before image assembly.

[org 0x20000]
bits 16

LOADER2_SEGMENT      equ 0x2000
KERNEL_OFFSET        equ 0x1000
KERNEL_SECTORS       equ __KERNEL_SECTORS__
STAGE2_STACK_TOP     equ __STAGE2_STACK_TOP__
STAGE2_STACK_TOP_32  equ __STAGE2_STACK_TOP_32__
BOOT_SECTOR_ADDR     equ 0x7C00
FAT16_LBA_PATCH_OFFSET equ 504

start:
    cli
    mov ax, LOADER2_SEGMENT
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STAGE2_STACK_TOP
    mov [BOOT_DRIVE], dl
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
    push ds
    xor ax, ax
    mov ds, ax
    mov eax, [BOOT_SECTOR_ADDR + FAT16_LBA_PATCH_OFFSET]
    pop ds
    sub eax, KERNEL_SECTORS
    mov dword [dap_lba],   eax
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
    ; loader2 now lives above 128 KiB, so the protected-mode far jump needs a
    ; 32-bit offset.  A 16-bit far jump would truncate init_pm and land in the
    ; wrong linear address.
    jmp dword CODE_SEG:init_pm

[bits 32]
init_pm:
    mov ax, DATA_SEG
    mov ds, ax
    mov ss, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ebp, STAGE2_STACK_TOP_32
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
