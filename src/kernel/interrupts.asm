[bits 32]

global gdt_flush
global idt_flush
global irq0_stub
global irq1_stub
global isr128_stub
global isr8_stub

extern irq0_handler_main
extern irq1_handler_main
extern syscall_handler_main

gdt_flush:
    mov eax, [esp + 4]
    lgdt [eax]
    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax
    mov ss, ax
    jmp 0x08:.flush
.flush:
    ret

idt_flush:
    mov eax, [esp + 4]
    lidt [eax]
    ret

irq0_stub:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call irq0_handler_main

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iretd

irq1_stub:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    call irq1_handler_main

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iretd

isr8_stub:
    mov byte [0xB8000 + (1 * 80 + 12) * 2], '8'
    mov byte [0xB8000 + (1 * 80 + 12) * 2 + 1], 0x4F
.hang8:
    cli
    hlt
    jmp .hang8

isr128_stub:
    pusha
    push ds
    push es
    push fs
    push gs

    mov ax, 0x10
    mov ds, ax
    mov es, ax
    mov fs, ax
    mov gs, ax

    push esp
    call syscall_handler_main
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iretd

; tss_flush(selector)
;
; Load the TSS selector into the Task Register.
; Called once from gdt_init() after the TSS descriptor is written.
;
; [esp+4] = TSS selector (e.g. 0x28)
;
global tss_flush
tss_flush:
    mov  eax, [esp+4]
    ltr  ax
    ret