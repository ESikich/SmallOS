bits 32

section .rodata

global EMBED_SYMBOL

EMBED_SYMBOL:
    incbin "EMBED_FILE"