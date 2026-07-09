#ifndef USER_SYS_PRCTL_H
#define USER_SYS_PRCTL_H

#define PR_SET_NAME 15
#define PR_GET_NAME 16

int prctl(int option, ...);

#endif /* USER_SYS_PRCTL_H */
