bits 32
section .text

global _start:function
extern ld_smallos_main

%define SYS_EXIT       2
%define PT_DYNAMIC     2
%define DT_NULL        0
%define DT_REL         17
%define DT_RELSZ       18
%define DT_RELENT      19
%define R_386_NONE     0
%define R_386_RELATIVE 8

_start:
    cld
    call .get_base
.get_base:
    pop ebx
    sub ebx, .get_base

    mov esi, [ebx + 28]       ; e_phoff
    add esi, ebx
    movzx ecx, word [ebx + 44] ; e_phnum
    movzx edx, word [ebx + 42] ; e_phentsize

.find_dynamic:
    test ecx, ecx
    jz .fail
    cmp dword [esi], PT_DYNAMIC
    je .dynamic_found
    add esi, edx
    dec ecx
    jmp .find_dynamic

.dynamic_found:
    mov edi, [esi + 8]        ; p_vaddr
    add edi, ebx
    xor esi, esi              ; DT_REL value
    xor ecx, ecx              ; DT_RELSZ value
    mov edx, 8                ; default Elf32_Rel size

.dynamic_loop:
    mov eax, [edi]
    test eax, eax
    jz .relocate
    cmp eax, DT_REL
    je .set_rel
    cmp eax, DT_RELSZ
    je .set_relsz
    cmp eax, DT_RELENT
    je .set_relent
.next_dynamic:
    add edi, 8
    jmp .dynamic_loop

.set_rel:
    mov esi, [edi + 4]
    jmp .next_dynamic

.set_relsz:
    mov ecx, [edi + 4]
    jmp .next_dynamic

.set_relent:
    mov edx, [edi + 4]
    jmp .next_dynamic

.relocate:
    test esi, esi
    jz .done
    cmp edx, 8
    jne .fail
    test ecx, 7
    jnz .fail
    add esi, ebx
    shr ecx, 3
    jz .done
    mov edi, esi

.rel_loop:
    mov eax, [edi + 4]
    and eax, 0xff
    cmp eax, R_386_NONE
    je .next_rel
    cmp eax, R_386_RELATIVE
    jne .fail
    mov eax, [edi]
    add eax, ebx
    add dword [eax], ebx

.next_rel:
    add edi, 8
    dec ecx
    jnz .rel_loop

.done:
    jmp ld_smallos_main

.fail:
    mov eax, SYS_EXIT
    mov ebx, 127
    int 0x80
.halt:
    hlt
    jmp .halt

section .note.GNU-stack noalloc noexec nowrite progbits
