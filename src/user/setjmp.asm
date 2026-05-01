bits 32
section .text

global setjmp
global longjmp

setjmp:
    mov  eax, [esp+4]
    mov  [eax+0],  ebx
    mov  [eax+4],  esi
    mov  [eax+8],  edi
    mov  [eax+12], ebp
    lea  ecx, [esp+4]
    mov  [eax+16], ecx
    mov  ecx, [esp]
    mov  [eax+20], ecx
    xor  eax, eax
    ret

longjmp:
    mov  eax, [esp+4]
    mov  ecx, [esp+8]
    mov  ebx, [eax+0]
    mov  esi, [eax+4]
    mov  edi, [eax+8]
    mov  ebp, [eax+12]
    mov  esp, [eax+16]
    mov  edx, [eax+20]
    push edx
    test ecx, ecx
    jnz  .nonzero
    mov  ecx, 1
.nonzero:
    mov  eax, ecx
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
