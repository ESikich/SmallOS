#ifndef USER_SETJMP_H
#define USER_SETJMP_H

typedef unsigned int jmp_buf[6];

int setjmp(jmp_buf env);
void longjmp(jmp_buf env, int val);

#endif
