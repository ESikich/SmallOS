#ifndef USER_SYS_IOCTL_H
#define USER_SYS_IOCTL_H

struct winsize {
    unsigned short ws_row;
    unsigned short ws_col;
    unsigned short ws_xpixel;
    unsigned short ws_ypixel;
};

#define TIOCGWINSZ 0x5413
#define TIOCSWINSZ 0x5414
#define TIOCGPGRP  0x540F
#define TIOCSPGRP  0x5410
#define TIOCSCTTY  0x540E
#define TIOCNOTTY  0x5422

int ioctl(int fd, unsigned long request, ...);

#endif /* USER_SYS_IOCTL_H */
