; loader2.asm
;
; Stage 2 loader contract:
;   - loaded by boot sector to physical 0x40000
;   - occupies exactly 8 sectors (4096 bytes)
;   - kernel and ext2 partitions are described by the MBR partition table
;   - kernel is loaded to physical 0x1000
;   - versioned boot info is written at physical 0x90000
;   - after loading, stage 2 enters 32-bit protected mode
;   - then jumps to kernel entry at 0x1000

[org 0x40000]
bits 16

LOADER2_SEGMENT      equ 0x4000
KERNEL_OFFSET        equ 0x1000
KERNEL_SEGMENT       equ KERNEL_OFFSET / 16
KERNEL_READ_CHUNK    equ 120        ; 120 sectors = 61440 bytes, fits below 64 KiB
STAGE2_STACK_TOP     equ __STAGE2_STACK_TOP__
STAGE2_STACK_TOP_32  equ __STAGE2_STACK_TOP_32__
FORCE_VGA_BACKEND    equ __FORCE_VGA_BACKEND__
BOOT_SECTOR_ADDR     equ 0x7C00
BOOT_INFO_SEG        equ 0x9000
BOOT_FONT_SEG        equ 0x9100
BOOT_INFO_MAGIC      equ 0x534D4F53
BOOT_INFO_VERSION    equ 2
BOOT_E820_MAX        equ 32
BOOT_INFO_SIZE_BYTES equ 816
BOOT_INFO_DWORDS     equ BOOT_INFO_SIZE_BYTES / 4
BOOT_FB_PHYS_OFF     equ 12
BOOT_FB_WIDTH_OFF    equ 16
BOOT_FB_HEIGHT_OFF   equ 20
BOOT_FB_PITCH_OFF    equ 24
BOOT_FB_BPP_OFF      equ 28
BOOT_FB_FORMAT_OFF   equ 32
BOOT_FB_VALID_OFF    equ 36
BOOT_E820_COUNT_OFF  equ 40
BOOT_E820_VALID_OFF  equ 44
BOOT_E820_TABLE_OFF  equ 48
BOOT_E820_ENTRY_SIZE equ 24
BOOT_FB_FORMAT_XRGB8888 equ 1
MBR_PARTITION_TABLE_OFFSET equ 446
MBR_PARTITION_ENTRY_SIZE   equ 16
MBR_PARTITION_LBA_OFFSET   equ 8
MBR_PARTITION_SIZE_OFFSET  equ 12
KERNEL_PARTITION_INDEX     equ 0

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

%if FORCE_VGA_BACKEND
    call vga_setup
%else
    call vbe_setup
%endif
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
    push ds
    xor ax, ax
    mov ds, ax
    mov eax, [BOOT_SECTOR_ADDR + MBR_PARTITION_TABLE_OFFSET + KERNEL_PARTITION_INDEX * MBR_PARTITION_ENTRY_SIZE + MBR_PARTITION_LBA_OFFSET]
    mov edx, [BOOT_SECTOR_ADDR + MBR_PARTITION_TABLE_OFFSET + KERNEL_PARTITION_INDEX * MBR_PARTITION_ENTRY_SIZE + MBR_PARTITION_SIZE_OFFSET]
    pop ds
    test dx, dx
    jz disk_error

    mov bx, KERNEL_SEGMENT

.read_loop:
    mov cx, dx
    cmp cx, KERNEL_READ_CHUNK
    jbe .chunk_ok
    mov cx, KERNEL_READ_CHUNK

.chunk_ok:
    mov word [dap_count], cx
    mov word [dap_seg], bx
    mov word [dap_off], 0x0000
    mov dword [dap_lba], eax
    mov dword [dap_lba+4], 0
    call lba_read

    add eax, ecx
    push cx
    shl cx, 5              ; 512 bytes per sector / 16 bytes per paragraph
    add bx, cx
    pop cx
    sub dx, cx
    jnz .read_loop

    popa
    ret

disk_error:
    mov si, disk_msg
    call print_string
    jmp $

; ---------------------------
init_boot_info:
    pusha
    push es
    mov ax, BOOT_INFO_SEG
    mov es, ax
    xor di, di
    xor eax, eax
    mov cx, BOOT_INFO_DWORDS
    cld
    rep stosd
    mov dword [es:0], BOOT_INFO_MAGIC
    mov dword [es:4], BOOT_INFO_VERSION
    mov dword [es:8], BOOT_INFO_SIZE_BYTES
    pop es
    popa
    ret

collect_e820:
    pusha
    push es

    mov ax, BOOT_INFO_SEG
    mov es, ax
    xor ebx, ebx
    xor bp, bp
    mov di, BOOT_E820_TABLE_OFF

.next:
    cmp bp, BOOT_E820_MAX
    jae .done

    mov eax, 0x0000E820
    mov edx, 0x534D4150     ; 'SMAP'
    mov ecx, BOOT_E820_ENTRY_SIZE
    mov dword [es:di + 20], 1
    int 0x15
    jc .done
    cmp eax, 0x534D4150
    jne .done
    cmp ecx, 20
    jb .done
    cmp ecx, BOOT_E820_ENTRY_SIZE
    jae .length_check
    mov dword [es:di + 20], 0

.length_check:
    mov eax, [es:di + 8]
    or eax, [es:di + 12]
    jz .skip_entry

    inc bp
    add di, BOOT_E820_ENTRY_SIZE

.skip_entry:
    test ebx, ebx
    jnz .next

.done:
    xor eax, eax
    mov ax, bp
    mov [es:BOOT_E820_COUNT_OFF], eax
    test bp, bp
    jz .no_entries
    mov dword [es:BOOT_E820_VALID_OFF], 1

.no_entries:
    pop es
    popa
    ret

copy_vga_font:
    pusha
    push ds
    push es

    mov ax, 0x1130
    mov bh, 0x06            ; 8x16 ROM font
    int 0x10

    push es
    pop ds
    mov si, bp
    mov ax, BOOT_FONT_SEG
    mov es, ax
    xor di, di
    mov cx, 4096            ; 256 glyphs * 16 bytes each
    cld
    rep movsb

    pop es
    pop ds
    popa
    ret

vga_setup:
    pusha
    call init_boot_info
    call collect_e820
    mov ax, 0x0003
    int 0x10
    call copy_vga_font
    popa
    ret

vbe_setup:
    pusha
    push ds
    push es

    call init_boot_info
    call collect_e820
    call copy_vga_font

    mov ax, LOADER2_SEGMENT
    mov ds, ax
    mov es, ax
    mov di, vbe_info
    mov byte [vbe_info + 0], 'V'
    mov byte [vbe_info + 1], 'B'
    mov byte [vbe_info + 2], 'E'
    mov byte [vbe_info + 3], '2'
    mov ax, 0x4F00
    int 0x10
    cmp ax, 0x004F
    jne .done

    mov si, [vbe_info + 0x0E]
    mov ax, [vbe_info + 0x10]
    mov es, ax

.next_mode:
    mov bx, [es:si]
    cmp bx, 0xFFFF
    je .done
    add si, 2

    mov [vbe_candidate_mode], bx
    push es
    push si
    mov ax, LOADER2_SEGMENT
    mov es, ax
    mov di, vbe_mode_info
    mov cx, bx
    mov ax, 0x4F01
    int 0x10
    pop si
    pop es

    mov bx, LOADER2_SEGMENT
    mov ds, bx
    cmp ax, 0x004F
    jne .next_mode

    mov ax, [vbe_mode_info + 0x00]
    test ax, 0x0001         ; supported
    jz .next_mode
    test ax, 0x0010         ; graphics mode
    jz .next_mode
    test ax, 0x0080         ; linear framebuffer
    jz .next_mode
    cmp word [vbe_mode_info + 0x12], 1024
    jne .next_mode
    cmp word [vbe_mode_info + 0x14], 768
    jne .next_mode
    cmp byte [vbe_mode_info + 0x19], 32
    jne .next_mode
    cmp dword [vbe_mode_info + 0x28], 0
    je .next_mode

    mov bx, [vbe_candidate_mode]
    or bx, 0x4000           ; request linear framebuffer
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .done

    mov ax, LOADER2_SEGMENT
    mov ds, ax
    mov ax, BOOT_INFO_SEG
    mov es, ax

    mov eax, [vbe_mode_info + 0x28]
    mov [es:BOOT_FB_PHYS_OFF], eax
    xor eax, eax
    mov ax, [vbe_mode_info + 0x12]
    mov [es:BOOT_FB_WIDTH_OFF], eax
    xor eax, eax
    mov ax, [vbe_mode_info + 0x14]
    mov [es:BOOT_FB_HEIGHT_OFF], eax
    xor eax, eax
    mov ax, [vbe_mode_info + 0x10]
    mov [es:BOOT_FB_PITCH_OFF], eax
    xor eax, eax
    mov al, [vbe_mode_info + 0x19]
    mov [es:BOOT_FB_BPP_OFF], eax
    mov dword [es:BOOT_FB_FORMAT_OFF], BOOT_FB_FORMAT_XRGB8888
    mov dword [es:BOOT_FB_VALID_OFF], 1

.done:
    pop es
    pop ds
    popa
    ret

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
vbe_candidate_mode   dw 0
ext_ok_msg           db "LBA ok ", 0
no_ext_msg           db "NO LBA!", 0
loader_msg           db "Loading...", 0
kernel_loaded_msg    db "K", 13, 10, 0
disk_msg             db "Disk err!", 0
align 4
vbe_info             times 512 db 0
vbe_mode_info        times 256 db 0

LOADER2_SIZE_BYTES equ 4096
times LOADER2_SIZE_BYTES-($-$$) db 0
