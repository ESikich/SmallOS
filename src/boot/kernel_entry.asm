[bits 32]
global _start
extern kernel_main
extern bss_start
extern bss_end

_start:
    mov edi, bss_start
    mov ecx, bss_end
    sub ecx, edi
    xor eax, eax
    rep stosb
    call kernel_main

hang:
    cli
    hlt
    jmp hang

section .note.GNU-stack noalloc noexec nowrite progbits
