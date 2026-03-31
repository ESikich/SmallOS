; sched_switch.asm — low-level context switch for the round-robin scheduler
;
; void sched_switch(unsigned int* save_esp,   [esp+4]
;                   unsigned int  next_esp,   [esp+8]
;                   unsigned int  next_cr3,   [esp+12]
;                   unsigned int  next_esp0); [esp+16]
;
; Steps (all args loaded into registers BEFORE modifying ESP):
;   1. Load all four arguments into EAX/EBX/ECX/EDX.
;   2. Save the current ESP into *save_esp (EAX).
;   3. Update TSS.ESP0 = next_esp0 (EDX) via tss_esp0_ptr.
;   4. Load next_cr3 (ECX) into CR3 — flushes TLB.
;   5. Set ESP = next_esp (EBX) — now on incoming context's kernel stack.
;   6. RET — pops the return address that the incoming context left on its
;      stack the last time it was suspended, resuming it transparently.
;
; Caller guarantees:
;   - Interrupts are disabled (inside IRQ handler).
;   - Both contexts' kernel stacks are identity-mapped and accessible.
;   - next_esp != 0 (caller checks before invoking).

bits 32
section .text

global sched_switch

extern tss_esp0_ptr   ; u32* pointing at tss.esp0, set by sched_init()

sched_switch:
    ; Load all arguments before touching ESP.
    mov  eax, [esp+4]      ; eax = save_esp  (out-pointer for current ESP)
    mov  ebx, [esp+8]      ; ebx = next_esp
    mov  ecx, [esp+12]     ; ecx = next_cr3
    mov  edx, [esp+16]     ; edx = next_esp0

    ; Save current ESP. At this point ESP points at the return address
    ; on the outgoing context's kernel stack (standard cdecl frame).
    ; When this context is next switched back to, ret will pop that
    ; address and execution continues in sched_tick.
    mov  [eax], esp

    ; Update TSS.ESP0 before CR3 switch so any fault in the window
    ; uses the correct kernel stack for the incoming process.
    mov  eax, [tss_esp0_ptr]
    mov  [eax], edx

    ; Switch page directory.
    mov  cr3, ecx

    ; Switch to incoming context's kernel stack, then resume it.
    mov  esp, ebx
    ret