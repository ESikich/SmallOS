[bits 32]

global gdt_flush
global idt_flush
global irq0_stub
global irq1_stub
global irq5_stub
global irq12_stub
global isr0_stub
global isr1_stub
global isr2_stub
global isr3_stub
global isr4_stub
global isr5_stub
global isr6_stub
global isr7_stub
global isr13_stub
global isr14_stub
global isr128_stub
global isr8_stub
global isr10_stub
global isr11_stub
global isr12_stub
global isr16_stub
global isr17_stub
global isr19_stub
global isr20_stub

extern irq0_handler_main
extern irq1_handler_main
extern irq5_handler_main
extern irq12_handler_main
extern fault_handler_main
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

%macro ISR_NOERR 1
isr%1_stub:
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

    mov eax, esp
    push dword %1
    push eax
    call fault_handler_main
    add esp, 8

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iretd
%endmacro

%macro ISR_ERR 1
isr%1_stub:
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

    mov eax, esp
    push dword %1
    push eax
    call fault_handler_main
    add esp, 8

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 4
    iretd
%endmacro

; irq0_stub — timer IRQ handler
;
; Passes the current kernel ESP to irq0_handler_main so the scheduler
; can save it as the preempted context's stack pointer.
;
; Stack layout after pusha + segment pushes (same as isr128_stub):
;
;   [esp]    = gs        (last pushed)
;   [esp+4]  = fs
;   [esp+8]  = es
;   [esp+12] = ds
;   [esp+16] = edi       \
;   [esp+20] = esi        |
;   [esp+24] = ebp        | pusha frame
;   [esp+28] = (orig esp) |
;   [esp+32] = ebx        |
;   [esp+36] = edx        |
;   [esp+40] = ecx        |
;   [esp+44] = eax       /
;   [esp+48] = eip       \  pushed by CPU on interrupt
;   [esp+52] = cs         |
;   [esp+56] = eflags    /
;   (if ring-3 → ring-0 transition, CPU also pushes esp and ss)
;
; We pass esp (after the frame is fully built) to irq0_handler_main.
; The scheduler records this value and uses it to resume this context
; later via sched_switch.
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

    push esp                ; pass current esp to C — scheduler saves it
    call irq0_handler_main
    add esp, 4

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

    push esp
    call irq1_handler_main
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iretd

irq5_stub:
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
    call irq5_handler_main
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iretd

irq12_stub:
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
    call irq12_handler_main
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iretd

ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 19
ISR_ERR   20

isr8_stub:
    ; Double fault is an emergency stop.  Write a visible marker and halt
    ; because the CPU state is already compromised by the time vector 8 runs.
    mov byte [0xB8000 + (1 * 80 + 12) * 2], 'D'
    mov byte [0xB8000 + (1 * 80 + 12) * 2 + 1], 0x4F
    mov byte [0xB8000 + (1 * 80 + 13) * 2], 'F'
    mov byte [0xB8000 + (1 * 80 + 13) * 2 + 1], 0x4F
    mov byte [0xB8000 + (1 * 80 + 14) * 2], '!'
    mov byte [0xB8000 + (1 * 80 + 14) * 2 + 1], 0x4F
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

section .note.GNU-stack noalloc noexec nowrite progbits
