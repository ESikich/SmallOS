; setjmp.asm — bare-metal i686 setjmp / longjmp
;
; Assembled with nasm, linked into the kernel.
;
; jmp_buf is an array of 6 dwords:
;   [env+0]   ebx
;   [env+4]   esi
;   [env+8]   edi
;   [env+12]  ebp
;   [env+16]  esp  (caller's esp — points at the return address on the stack)
;   [env+20]  eip  (return address — the instruction after the call to setjmp)

bits 32
section .text

global setjmp
global longjmp

; ---------------------------------------------------------------
; int setjmp(jmp_buf env)           [esp+4] = env
;
; Save callee-saved registers and the return address into env.
; Returns 0.
; ---------------------------------------------------------------
setjmp:
    mov  eax, [esp+4]          ; eax = env pointer

    mov  [eax+0],  ebx
    mov  [eax+4],  esi
    mov  [eax+8],  edi
    mov  [eax+12], ebp

    ; Save esp as it is at the call site (before call pushed ret addr).
    ; Currently esp points at [return_address, env, ...].
    ; We want to restore esp so that on longjmp the caller sees its
    ; own stack frame — so save the value esp had before 'call setjmp'
    ; which is esp+4 (skip the return address that 'call' pushed).
    lea  ecx, [esp+4]
    mov  [eax+16], ecx         ; saved esp (caller's esp before call)

    mov  ecx, [esp]            ; return address
    mov  [eax+20], ecx         ; saved eip

    xor  eax, eax              ; return 0
    ret

; ---------------------------------------------------------------
; void longjmp(jmp_buf env, int val)
;   [esp+4] = env
;   [esp+8] = val
;
; Restore registers and jump back to setjmp's call site.
; setjmp will "return" val (or 1 if val == 0).
; ---------------------------------------------------------------
longjmp:
    mov  eax, [esp+4]          ; eax = env pointer
    mov  ecx, [esp+8]          ; ecx = val

    ; Restore callee-saved registers.
    mov  ebx, [eax+0]
    mov  esi, [eax+4]
    mov  edi, [eax+8]
    mov  ebp, [eax+12]

    ; Restore esp to the caller's stack frame.
    mov  esp, [eax+16]

    ; Place return address on the restored stack and set return value.
    mov  edx, [eax+20]         ; saved eip (return address into setjmp caller)
    push edx                   ; set up for ret

    ; Return value: val, but never 0.
    test ecx, ecx
    jnz  .nonzero
    mov  ecx, 1
.nonzero:
    mov  eax, ecx

    ret                        ; jump to saved eip with eax = val

section .note.GNU-stack noalloc noexec nowrite progbits
