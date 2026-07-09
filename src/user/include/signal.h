#ifndef USER_SIGNAL_H
#define USER_SIGNAL_H

typedef unsigned int sigset_t;
typedef void (*sighandler_t)(int);
typedef struct siginfo siginfo_t;
typedef void (*sigaction_handler_t)(int, siginfo_t*, void*);

#define SIGHUP  1
#define SIGINT  2
#define SIGQUIT 3
#define SIGILL  4
#define SIGTRAP 5
#define SIGABRT 6
#define SIGBUS  7
#define SIGFPE  8
#define SIGKILL 9
#define SIGUSR1 10
#define SIGSEGV 11
#define SIGUSR2 12
#define SIGPIPE 13
#define SIGALRM 14
#define SIGTERM 15
#define SIGCHLD 17
#define SIGCONT 18
#define SIGSTOP 19
#define SIGTSTP 20
#define SIGTTIN 21
#define SIGTTOU 22
#define SIGURG  23
#define SIGXCPU 24
#define SIGXFSZ 25
#define SIGVTALRM 26
#define SIGPROF 27
#define SIGWINCH 28
#define NSIG 32

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

struct siginfo {
    int si_signo;
    int si_errno;
    int si_code;
    void* si_addr;
};

struct sigaction {
    union {
        sighandler_t handler;
        sigaction_handler_t sigaction;
    } __sa_handler;
    sigset_t sa_mask;
    int sa_flags;
};

#define sa_handler __sa_handler.handler
#define sa_sigaction __sa_handler.sigaction

#define SA_NOCLDSTOP 1
#define SA_SIGINFO 4
#define SA_RESTART 0x10000000

#define SI_KERNEL 128
#define SEGV_MAPERR 1
#define SEGV_ACCERR 2
#define FPE_INTDIV 1
#define FPE_FLTDIV 3
#define ILL_ILLOPC 1
#define BUS_ADRERR 2

struct timespec;

int sigemptyset(sigset_t* set);
int sigaddset(sigset_t* set, int signum);
int sigdelset(sigset_t* set, int signum);
int sigfillset(sigset_t* set);
int sigprocmask(int how, const sigset_t* set, sigset_t* oldset);
int sigsuspend(const sigset_t* mask);
int sigtimedwait(const sigset_t* set, void* info, const struct timespec* timeout);
sighandler_t signal(int signum, sighandler_t handler);
int sigaction(int signum, const struct sigaction* act, struct sigaction* oldact);
int raise(int signum);
int kill(int pid, int signum);
int killpg(int pgrp, int signum);

#endif /* USER_SIGNAL_H */
