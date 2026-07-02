#ifndef USER_TERMIOS_H
#define USER_TERMIOS_H

typedef unsigned int tcflag_t;
typedef unsigned char cc_t;
typedef unsigned int speed_t;

#define NCCS 32

#define VINTR 0
#define VQUIT 1
#define VERASE 2
#define VKILL 3
#define VEOF 4
#define VTIME 5
#define VMIN 6
#define VSWTC 7
#define VSTART 8
#define VSTOP 9
#define VSUSP 10
#define VEOL 11
#define VREPRINT 12
#define VDISCARD 13
#define VWERASE 14
#define VLNEXT 15
#define VEOL2 16

struct termios {
    tcflag_t c_iflag;
    tcflag_t c_oflag;
    tcflag_t c_cflag;
    tcflag_t c_lflag;
    cc_t c_cc[NCCS];
};

#define TCSANOW 0
#define TCSADRAIN 1
#define TCSAFLUSH 2

#define TCIFLUSH 0
#define TCOFLUSH 1
#define TCIOFLUSH 2

#define B0 0u
#define B50 50u
#define B75 75u
#define B110 110u
#define B134 134u
#define B150 150u
#define B200 200u
#define B300 300u
#define B600 600u
#define B1200 1200u
#define B1800 1800u
#define B2400 2400u
#define B4800 4800u
#define B9600 9600u
#define B19200 19200u
#define B38400 38400u
#define B57600 57600u
#define B115200 115200u

#define ECHO 0000010
#define ECHOE 0000020
#define ECHOK 0000040
#define ECHONL 0000100
#define ICANON 0000002
#define ISIG 0000001
#define IEXTEN 0100000
#define OPOST 0000001
#define BRKINT 0000002
#define ICRNL 0000400
#define INLCR 0000100
#define IXON 0002000
#define IXOFF 0010000
#define IXANY 0004000
#define IUCLC 0001000
#define IMAXBEL 0020000
#define ONLCR 0000004

int tcgetattr(int fd, struct termios* termios_p);
int tcsetattr(int fd, int optional_actions, const struct termios* termios_p);
int tcflush(int fd, int queue_selector);

#endif /* USER_TERMIOS_H */
