; loader2.asm
;
; Stage 2 loader contract:
;   - loaded by boot sector to physical 0x40000
;   - occupies exactly 16 sectors (8192 bytes)
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
RAMDISK_READ_CHUNK   equ 120        ; keep BIOS reads below the 64 KiB DMA boundary
STAGE2_STACK_TOP     equ __STAGE2_STACK_TOP__
STAGE2_STACK_TOP_32  equ __STAGE2_STACK_TOP_32__
FORCE_VGA_BACKEND    equ __FORCE_VGA_BACKEND__
FORCE_CHS_BOOT       equ __FORCE_CHS_BOOT__
VBE_DIAG             equ __VBE_DIAG__
VBE_RELAXED          equ __VBE_RELAXED__
BOOT_SECTOR_ADDR     equ 0x7C00
BOOT_INFO_SEG        equ 0x9000
BOOT_FONT_SEG        equ 0x9100
BOOT_INFO_MAGIC      equ 0x534D4F53
BOOT_INFO_VERSION    equ 3
BOOT_E820_MAX        equ 32
BOOT_INFO_SIZE_BYTES equ 832
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
BOOT_RAMDISK_PHYS_OFF  equ 816
BOOT_RAMDISK_SIZE_OFF  equ 820
BOOT_RAMDISK_VALID_OFF equ 824
BOOT_FB_FORMAT_XRGB8888 equ 1
MBR_PARTITION_TABLE_OFFSET equ 446
MBR_PARTITION_ENTRY_SIZE   equ 16
MBR_PARTITION_LBA_OFFSET   equ 8
MBR_PARTITION_SIZE_OFFSET  equ 12
KERNEL_PARTITION_INDEX     equ 0
EXT2_PARTITION_INDEX       equ 1
RAMDISK_PHYS               equ 0x00800000
RAMDISK_BOUNCE_SEG         equ 0x5000
RAMDISK_BOUNCE_PHYS        equ 0x00050000

start:
    cli
    mov ax, LOADER2_SEGMENT
    mov ds, ax
    mov es, ax
    mov ss, ax
    mov sp, STAGE2_STACK_TOP
    mov [BOOT_DRIVE], dl
    sti

    call init_serial
    mov byte [DISK_USE_LBA], 0

%if FORCE_CHS_BOOT
    mov si, forced_chs_msg
    call print_string
    call read_chs_geometry
    call print_drive_diag
    jmp .load
%endif

    ; --- Check INT 0x13 extensions ---
    mov ah, 0x41
    mov bx, 0x55AA
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc .no_ext
    cmp bx, 0xAA55
    jne .no_ext
    test cx, 0x0001
    jz .no_ext

    mov si, ext_ok_msg
    call print_string
    mov byte [DISK_USE_LBA], 1
    call read_chs_geometry
    call print_drive_diag
    jmp .load

.no_ext:
    mov si, no_ext_msg
    call print_string
    call read_chs_geometry
    call print_drive_diag

.load:
    mov si, loader_msg
    call print_string

    call cache_ramdisk_layout
    call load_kernel

    mov si, kernel_loaded_msg
    call print_string

    call enable_a20
    call load_ramdisk
%if FORCE_VGA_BACKEND
    call vga_setup
%else
    call vbe_setup
%endif
    call publish_ramdisk
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
    call print_char
    jmp .loop
.done:
    popa
    ret

init_serial:
    pusha
    mov dx, 0x3F8 + 1
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x80
    out dx, al
    mov dx, 0x3F8 + 0
    mov al, 0x03            ; 38400 baud divisor
    out dx, al
    mov dx, 0x3F8 + 1
    xor al, al
    out dx, al
    mov dx, 0x3F8 + 3
    mov al, 0x03            ; 8N1
    out dx, al
    mov dx, 0x3F8 + 2
    mov al, 0xC7
    out dx, al
    mov dx, 0x3F8 + 4
    mov al, 0x0B
    out dx, al
    popa
    ret

enable_a20:
    push ax
    in al, 0x92
    or al, 0x02
    and al, 0xFE
    out 0x92, al
    pop ax
    ret

; ---------------------------
lba_read:
    pushad
    push ds
    push es
    push fs
    push gs
    mov ax, LOADER2_SEGMENT
    mov fs, ax
    mov gs, ax
    mov ah, 0x42
    mov dl, [BOOT_DRIVE]
    mov si, dap
    int 0x13
    mov [DISK_ERROR_CODE], ah
    pop gs
    pop fs
    pop es
    pop ds
    jc disk_error
    popad
    ret

disk_read:
    cmp byte [DISK_USE_LBA], 0
    je chs_read_sector
    jmp lba_read

read_chs_geometry:
    pusha
    push es
    xor ax, ax
    mov es, ax
    mov ah, 0x08
    mov dl, [BOOT_DRIVE]
    int 0x13
    jc .fallback
    xor ax, ax
    mov al, cl
    and al, 0x3F
    jz .fallback
    mov [CHS_SECTORS_PER_TRACK], ax
    xor ax, ax
    mov al, dh
    inc ax
    mov [CHS_HEADS], ax
    jmp .done
.fallback:
    mov word [CHS_SECTORS_PER_TRACK], 63
    mov word [CHS_HEADS], 16
.done:
    pop es
    popa
    ret

chs_read_sector:
    pushad
    push ds
    push es
    call lba_to_chs
    mov [DISK_LAST_LBA], eax
    push fs
    push gs
    mov ax, LOADER2_SEGMENT
    mov fs, ax
    mov gs, ax
    xor ax, ax
    mov dl, [BOOT_DRIVE]
    int 0x13
    mov ax, [CHS_CYLINDER]
    cmp ax, 1023
    ja disk_error
    mov ch, al
    mov cl, [CHS_SECTOR]
    mov al, ah
    and al, 0x03
    shl al, 6
    or cl, al
    mov dh, [CHS_HEAD]
    mov dl, [BOOT_DRIVE]
    mov ah, 0x02
    mov al, [dap_count]
    int 0x13
    mov [DISK_ERROR_CODE], ah
    pop gs
    pop fs
    pop es
    pop ds
    jc disk_error
    popad
    ret

lba_to_chs:
    push eax
    push ebx
    push edx

    xor edx, edx
    xor ebx, ebx
    mov bx, [CHS_SECTORS_PER_TRACK]
    div ebx
    inc dl
    mov [CHS_SECTOR], dl

    xor edx, edx
    xor ebx, ebx
    mov bx, [CHS_HEADS]
    div ebx
    mov [CHS_CYLINDER], ax
    mov [CHS_HEAD], dl

    pop edx
    pop ebx
    pop eax
    ret

chs_cap_track_chunk:
    push eax
    push ebx
    push edx

    xor edx, edx
    xor ebx, ebx
    mov bx, [CHS_SECTORS_PER_TRACK]
    div ebx
    mov ax, [CHS_SECTORS_PER_TRACK]
    sub ax, dx
    cmp cx, ax
    jbe .done
    mov cx, ax
.done:
    pop edx
    pop ebx
    pop eax
    ret

cache_ramdisk_layout:
    pusha
    push ds
    push es
    xor ax, ax
    mov ds, ax
    mov ax, LOADER2_SEGMENT
    mov es, ax
    mov eax, [BOOT_SECTOR_ADDR + MBR_PARTITION_TABLE_OFFSET + EXT2_PARTITION_INDEX * MBR_PARTITION_ENTRY_SIZE + MBR_PARTITION_LBA_OFFSET]
    mov [es:RAMDISK_LBA], eax
    mov eax, [BOOT_SECTOR_ADDR + MBR_PARTITION_TABLE_OFFSET + EXT2_PARTITION_INDEX * MBR_PARTITION_ENTRY_SIZE + MBR_PARTITION_SIZE_OFFSET]
    mov [es:RAMDISK_SECTORS], eax
    pop es
    pop ds
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
    xor ecx, ecx
    mov cx, dx
    cmp byte [DISK_USE_LBA], 0
    jne .cap_lba
    cmp cx, 1
    jbe .chunk_ok
    mov cx, 1
    jmp .chunk_ok
.cap_lba:
    cmp cx, KERNEL_READ_CHUNK
    jbe .chunk_ok
    mov cx, KERNEL_READ_CHUNK

.chunk_ok:
    mov word [dap_count], cx
    mov word [dap_seg], bx
    mov word [dap_off], 0x0000
    mov dword [dap_lba], eax
    mov dword [dap_lba+4], 0
    push bx
    cmp byte [DISK_USE_LBA], 0
    jne .read_now
    push eax
    mov ax, RAMDISK_BOUNCE_SEG
    mov es, ax
    pop eax
    xor bx, bx
.read_now:
    call disk_read
    pop bx
    cmp byte [DISK_USE_LBA], 0
    jne .read_done
    call copy_kernel_bounce_sector
.read_done:

    add eax, ecx
    push cx
    shl cx, 5              ; 512 bytes per sector / 16 bytes per paragraph
    add bx, cx
    pop cx
    sub dx, cx
    jnz .read_loop

    popa
    ret

load_ramdisk:
    pusha
    mov eax, [RAMDISK_LBA]
    mov edx, [RAMDISK_SECTORS]
    test dx, dx
    jz disk_error

    mov ebx, RAMDISK_PHYS
    mov dword [RAMDISK_SECTORS_READ], 0
    mov dword [RAMDISK_NEXT_DOT], 2048

.read_loop:
    xor ecx, ecx
    mov cx, dx
    cmp byte [DISK_USE_LBA], 0
    jne .cap_lba
    cmp cx, RAMDISK_READ_CHUNK
    jbe .cap_chs_track
    mov cx, RAMDISK_READ_CHUNK
.cap_chs_track:
    call chs_cap_track_chunk
    jmp .chunk_ok
.cap_lba:
    cmp cx, RAMDISK_READ_CHUNK
    jbe .chunk_ok
    mov cx, RAMDISK_READ_CHUNK

.chunk_ok:
    mov word [dap_count], cx
    mov word [dap_seg], RAMDISK_BOUNCE_SEG
    mov word [dap_off], 0x0000
    mov dword [dap_lba], eax
    mov dword [dap_lba+4], 0
    push ebx
    cmp byte [DISK_USE_LBA], 0
    jne .read_now
    push eax
    mov ax, RAMDISK_BOUNCE_SEG
    mov es, ax
    pop eax
    xor bx, bx
.read_now:
    call disk_read
    pop ebx
    call enter_unreal
    call copy_ramdisk_chunk

    add eax, ecx
    push eax
    xor eax, eax
    mov ax, cx
    shl eax, 9
    add ebx, eax
    pop eax
    call ramdisk_progress
    sub dx, cx
    jnz .read_loop

    mov eax, [RAMDISK_SECTORS]
    shl eax, 9
    mov [RAMDISK_SIZE_BYTES], eax

    mov si, ramdisk_loaded_msg
    call print_string

    popa
    ret

ramdisk_progress:
    push eax
    push edx
    xor eax, eax
    mov ax, cx
    add [RAMDISK_SECTORS_READ], eax
    mov eax, [RAMDISK_SECTORS_READ]
    cmp eax, [RAMDISK_NEXT_DOT]
    jb .done
    mov al, '.'
    call print_char
    add dword [RAMDISK_NEXT_DOT], 2048
.done:
    pop edx
    pop eax
    ret

publish_ramdisk:
    pusha
    mov ax, BOOT_INFO_SEG
    mov es, ax
    mov eax, [RAMDISK_SIZE_BYTES]
    test eax, eax
    jz .done
    mov dword [es:BOOT_RAMDISK_PHYS_OFF], RAMDISK_PHYS
    mov dword [es:BOOT_RAMDISK_SIZE_OFF], eax
    mov dword [es:BOOT_RAMDISK_VALID_OFF], 1
.done:
    popa
    ret

copy_ramdisk_chunk:
    push eax
    push ecx
    push esi
    push edi

    xor eax, eax
    mov ax, cx
    shl eax, 7              ; sector count * 512 / 4 dwords
    mov ecx, eax
    mov esi, RAMDISK_BOUNCE_PHYS
    mov edi, ebx

.copy_loop:
    test ecx, ecx
    jz .done
    mov eax, [fs:esi]
    mov [gs:edi], eax
    add esi, 4
    add edi, 4
    dec ecx
    jmp .copy_loop

.done:
    pop edi
    pop esi
    pop ecx
    pop eax
    ret

copy_kernel_bounce_sector:
    pusha
    push ds
    push es
    mov ax, RAMDISK_BOUNCE_SEG
    mov ds, ax
    mov ax, bx
    mov es, ax
    xor si, si
    xor di, di
    mov cx, 256
    cld
    rep movsw
    pop es
    pop ds
    popa
    ret

enter_unreal:
    cli
    push eax
    push ax

    lgdt [gdt_descriptor]
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax
    mov ax, DATA_SEG
    mov fs, ax
    mov gs, ax
    mov eax, cr0
    and eax, 0xFFFFFFFE
    mov cr0, eax

    mov ax, LOADER2_SEGMENT
    mov ds, ax
    mov es, ax

    pop ax
    pop eax
    sti
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
    mov si, lba_msg
    call print_string
    mov eax, [DISK_LAST_LBA]
    call print_hex32
    mov si, cyl_msg
    call print_string
    mov ax, [CHS_CYLINDER]
    call print_hex16
    mov si, head_msg
    call print_string
    xor ax, ax
    mov al, [CHS_HEAD]
    call print_hex16
    mov si, sec_msg
    call print_string
    xor ax, ax
    mov al, [CHS_SECTOR]
    call print_hex16
    jmp $

print_drive_diag:
    pusha
    mov si, drive_msg
    call print_string
    mov al, [BOOT_DRIVE]
    call print_hex8
    mov si, mode_msg
    call print_string
    mov al, [DISK_USE_LBA]
    call print_hex8
    mov si, heads_msg
    call print_string
    mov ax, [CHS_HEADS]
    call print_hex16
    mov si, spt_msg
    call print_string
    mov ax, [CHS_SECTORS_PER_TRACK]
    call print_hex16
    mov si, crlf_msg
    call print_string
    popa
    ret

print_hex16:
    push ax
    mov al, ' '
    call print_char
    mov al, '0'
    call print_char
    mov al, 'x'
    call print_char
    pop ax
    push ax
    mov al, ah
    call print_hex8_raw
    pop ax
    call print_hex8_raw
    ret

print_hex32:
    push eax
    shr eax, 16
    call print_hex16
    pop eax
    call print_hex16_raw
    ret

print_hex16_raw:
    push ax
    mov al, ah
    call print_hex8_raw
    pop ax
    call print_hex8_raw
    ret

print_hex8:
    push ax
    mov al, ' '
    call print_char
    mov al, '0'
    call print_char
    mov al, 'x'
    call print_char
    pop ax
    jmp print_hex8_raw

print_hex8_raw:
    push ax
    shr al, 4
    call print_hex_nibble
    pop ax
    and al, 0x0F
    call print_hex_nibble
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
    push ax
    push dx
    push ax
    mov dx, 0x3FD
.serial_wait:
    in al, dx
    test al, 0x20
    jz .serial_wait
    pop ax
    mov dx, 0x3F8
    out dx, al
    pop dx
    pop ax
    mov ah, 0x0E
    int 0x10
    ret

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

%if VBE_DIAG
    mov si, vbe_probe_msg
    call print_string
%endif

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
    jne .info_fail

%if VBE_DIAG
    mov si, vbe_ok_msg
    call print_string
    mov ax, [vbe_info + 0x04]
    call print_hex16
    mov si, vbe_mem_msg
    call print_string
    mov ax, [vbe_info + 0x12]
    call print_hex16
    mov si, crlf_msg
    call print_string
%endif

    mov si, [vbe_info + 0x0E]
    mov ax, [vbe_info + 0x10]
    mov es, ax

.next_mode:
    mov bx, [es:si]
    cmp bx, 0xFFFF
    je .no_mode
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
    cmp byte [vbe_mode_info + 0x19], 32
    jne .next_mode
    cmp dword [vbe_mode_info + 0x28], 0
    je .next_mode

%if VBE_DIAG
    call print_vbe_candidate
%endif

%if VBE_RELAXED
    cmp word [vbe_mode_info + 0x12], 1024
    jne .try_800
    cmp word [vbe_mode_info + 0x14], 768
    je .select_mode
.try_800:
    cmp word [vbe_mode_info + 0x12], 800
    jne .try_640
    cmp word [vbe_mode_info + 0x14], 600
    je .select_mode
.try_640:
    cmp word [vbe_mode_info + 0x12], 640
    jne .next_mode
    cmp word [vbe_mode_info + 0x14], 480
    jne .next_mode
%else
    cmp word [vbe_mode_info + 0x12], 1024
    jne .next_mode
    cmp word [vbe_mode_info + 0x14], 768
    jne .next_mode
%endif

.select_mode:
%if VBE_DIAG
    mov si, vbe_set_msg
    call print_string
    mov ax, [vbe_candidate_mode]
    call print_hex16
    mov si, crlf_msg
    call print_string
%endif
    mov bx, [vbe_candidate_mode]
    or bx, 0x4000           ; request linear framebuffer
    mov ax, 0x4F02
    int 0x10
    cmp ax, 0x004F
    jne .set_fail

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
    jmp .done

.info_fail:
%if VBE_DIAG
    mov si, vbe_info_fail_msg
    call print_string
    call print_hex16
    mov si, crlf_msg
    call print_string
%endif
    jmp .done

.set_fail:
%if VBE_DIAG
    mov si, vbe_set_fail_msg
    call print_string
    call print_hex16
    mov si, crlf_msg
    call print_string
%endif
    jmp .done

.no_mode:
%if VBE_DIAG
    mov si, vbe_no_mode_msg
    call print_string
%endif

.done:
    pop es
    pop ds
    popa
    ret

%if VBE_DIAG
print_vbe_candidate:
    pusha
    mov si, vbe_mode_msg
    call print_string
    mov ax, [vbe_candidate_mode]
    call print_hex16
    mov si, vbe_wh_msg
    call print_string
    mov ax, [vbe_mode_info + 0x12]
    call print_hex16
    mov al, 'x'
    call print_char
    mov ax, [vbe_mode_info + 0x14]
    call print_hex16
    mov si, vbe_bpp_msg
    call print_string
    xor ax, ax
    mov al, [vbe_mode_info + 0x19]
    call print_hex8
    mov si, vbe_pitch_msg
    call print_string
    mov ax, [vbe_mode_info + 0x10]
    call print_hex16
    mov si, vbe_fb_msg
    call print_string
    mov eax, [vbe_mode_info + 0x28]
    call print_hex32
    mov si, crlf_msg
    call print_string
    popa
    ret
%endif

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
DISK_USE_LBA         db 0
DISK_ERROR_CODE      db 0
CHS_SECTOR           db 0
CHS_HEAD             db 0
CHS_CYLINDER         dw 0
CHS_HEADS            dw 16
CHS_SECTORS_PER_TRACK dw 63
DISK_LAST_LBA        dd 0
vbe_candidate_mode   dw 0
RAMDISK_SIZE_BYTES   dd 0
RAMDISK_LBA          dd 0
RAMDISK_SECTORS      dd 0
RAMDISK_SECTORS_READ dd 0
RAMDISK_NEXT_DOT     dd 0
ext_ok_msg           db "LBA ok ", 0
no_ext_msg           db "NO LBA; CHS fallback ", 0
forced_chs_msg       db "FORCE CHS ", 0
loader_msg           db "Loading kernel... ", 0
kernel_loaded_msg    db "done", 13, 10, "Preloading ext2 fallback... ", 0
ramdisk_loaded_msg   db "done", 13, 10, 0
disk_msg             db "Disk err!", 0
drive_msg            db " drive=", 0
error_msg            db " err=", 0
lba_msg              db " lba=", 0
cyl_msg              db " c=", 0
head_msg             db " h=", 0
sec_msg              db " s=", 0
mode_msg             db " lba=", 0
heads_msg            db " heads=", 0
spt_msg              db " spt=", 0
vbe_probe_msg        db "VBE probe", 13, 10, 0
vbe_ok_msg           db "VBE ok ver=", 0
vbe_mem_msg          db " mem=", 0
vbe_info_fail_msg    db "VBE info fail ax=", 0
vbe_mode_msg         db "VBE mode=", 0
vbe_wh_msg           db " wh=", 0
vbe_bpp_msg          db " bpp=", 0
vbe_pitch_msg        db " pitch=", 0
vbe_fb_msg           db " fb=", 0
vbe_set_msg          db "VBE set mode=", 0
vbe_set_fail_msg     db "VBE set fail ax=", 0
vbe_no_mode_msg      db "VBE no usable 32bpp LFB mode", 13, 10, 0
crlf_msg             db 13, 10, 0
align 4
vbe_info             times 512 db 0
vbe_mode_info        times 256 db 0

LOADER2_SIZE_BYTES equ 8192
times LOADER2_SIZE_BYTES-($-$$) db 0
