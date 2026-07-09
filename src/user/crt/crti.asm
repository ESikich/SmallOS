bits 32

section .init
global _init:function
_init:
    ret

section .fini
global _fini:function
_fini:
    ret

section .note.GNU-stack noalloc noexec nowrite progbits
