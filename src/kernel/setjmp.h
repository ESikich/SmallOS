#ifndef SETJMP_H
#define SETJMP_H

/*
 * Minimal setjmp/longjmp for the i686 freestanding kernel.
 *
 * jmp_buf layout (6 x 32-bit slots):
 *
 *   [0]  ebx
 *   [1]  esi
 *   [2]  edi
 *   [3]  ebp
 *   [4]  esp   (value at call site — points to return address)
 *   [5]  eip   (return address — where setjmp will "return" on longjmp)
 *
 * These are the callee-saved registers in the i386 System V ABI.
 * Saving/restoring them is sufficient to resume execution in the
 * calling function.
 *
 * Implemented in setjmp.asm (pure assembly — no C frame needed).
 */

typedef unsigned int jmp_buf[6];

/*
 * setjmp(env)
 *
 * Save the calling context into env.
 * Returns 0 when called directly.
 * Returns val (never 0) when resumed by longjmp().
 */
int setjmp(jmp_buf env);

/*
 * longjmp(env, val)
 *
 * Restore the context saved by setjmp() and cause setjmp() to return val.
 * If val == 0, setjmp() returns 1 instead (POSIX requirement).
 * Does not return to its caller.
 */
void longjmp(jmp_buf env, int val);

#endif