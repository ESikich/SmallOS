#ifndef SYSCALL_H
#define SYSCALL_H

#include <stddef.h>
#include "uapi_syscall.h"

/*
 * Saved register frame for int 0x80.
 *
 * IMPORTANT:
 * This layout must match isr128_stub exactly.
 *
 * Current assembly push order in interrupts.asm:
 *   pusha
 *   push ds
 *   push es
 *   push fs
 *   push gs
 *   push esp        ; pointer to saved syscall frame passed to C
 *
 * Because the stack grows downward, the C-visible struct begins with:
 *   gs, fs, es, ds,
 *   edi, esi, ebp, esp, ebx, edx, ecx, eax
 *
 * If isr128_stub changes, this struct must be updated to match.
 */
typedef struct syscall_regs {
    unsigned int gs;
    unsigned int fs;
    unsigned int es;
    unsigned int ds;

    unsigned int edi;
    unsigned int esi;
    unsigned int ebp;
    unsigned int esp;
    unsigned int ebx;
    unsigned int edx;
    unsigned int ecx;
    unsigned int eax;
} syscall_regs_t;

/*
 * Main syscall dispatcher.
 *
 * Input:
 *   regs->eax = syscall number
 *   regs->ebx = arg1
 *   regs->ecx = arg2
 *   regs->edx = arg3
 *
 * Output:
 *   regs->eax = return value
 */
void syscall_handler_main(syscall_regs_t* regs);

#endif