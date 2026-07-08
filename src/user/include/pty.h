#ifndef USER_PTY_H
#define USER_PTY_H

#include "termios.h"
#include "sys/ioctl.h"

int openpty(int* amaster,
            int* aslave,
            char* name,
            const struct termios* termp,
            const struct winsize* winp);
int login_tty(int fd);

#endif /* USER_PTY_H */
