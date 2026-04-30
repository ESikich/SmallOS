[bits 32]

global gdt_flush
global idt_flush
global irq0_stub
global irq1_stub
global isr6_stub
global isr13_stub
global isr14_stub
global isr128_stub
global isr8_stub

extern irq0_handler_main
extern irq1_handler_main
extern invalid_opcode_handler_main
extern general_protection_handler_main
extern page_fault_handler_main
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

    call irq1_handler_main

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iretd

isr6_stub:
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
    call invalid_opcode_handler_main
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    iretd

isr13_stub:
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
    call general_protection_handler_main
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 4
    iretd

isr14_stub:
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
    call page_fault_handler_main
    add esp, 4

    pop gs
    pop fs
    pop es
    pop ds
    popa
    add esp, 4
    iretd

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
