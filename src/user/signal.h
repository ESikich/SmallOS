#ifndef USER_SIGNAL_H
#define USER_SIGNAL_H

typedef unsigned int sigset_t;
typedef void (*sighandler_t)(int);

#define SIGHUP  1
#define SIGINT  2
#define SIGTERM 15
#define SIGPIPE 13

#define SIG_BLOCK   0
#define SIG_UNBLOCK 1
#define SIG_SETMASK 2

#define SIG_DFL ((sighandler_t)0)
#define SIG_IGN ((sighandler_t)1)

int sigemptyset(sigset_t* set);
int sigaddset(sigset_t* set, int signum);
int sigprocmask(int how, const sigset_t* set, sigset_t* oldset);
sighandler_t signal(int signum, sighandler_t handler);
int kill(int pid, int signum);

#endif /* USER_SIGNAL_H */
