bits 32
section .text

global sched_switch

extern tss_set_kernel_stack

sched_switch:
    ; Load all arguments before touching ESP.
    mov  eax, [esp+4]      ; save_esp
    mov  ebx, [esp+8]      ; next_esp
    mov  ecx, [esp+12]     ; next_cr3
    mov  edx, [esp+16]     ; next_esp0

    ; Save current ESP
    mov  [eax], esp

    ; Update TSS.ESP0 via C helper (keeps TSS encapsulated)
    push edx
    call tss_set_kernel_stack
    add  esp, 4

    ; Switch page directory
    mov  cr3, ecx

    ; Switch to next kernel stack
    mov  esp, ebx
    ret