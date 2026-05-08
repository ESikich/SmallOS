#ifndef UAPI_SYSCALL_H
#define UAPI_SYSCALL_H

/*
 * SmallOS syscall ABI v1
 *
 * Invocation:
 *   int 0x80
 *
 * Register convention:
 *   eax = syscall number
 *   ebx = arg1
 *   ecx = arg2
 *   edx = arg3
 *   esi = arg4 for four-argument calls
 *
 * Return value:
 *   eax = result
 *
 * Error convention:
 *   negative value means error
 *   failures should use -errno values from uapi_errno.h
 *
 * Notes:
 *   - This ABI is currently used by user-space ELF programs.
 *   - Keep this file shared between kernel and ELF-side code.
 */

#define SYSCALL_ABI_VERSION 1

#define SYS_OPEN_MODE_READ   0x01u
#define SYS_OPEN_MODE_WRITE  0x02u
#define SYS_OPEN_MODE_CREATE 0x04u
#define SYS_OPEN_MODE_TRUNC  0x08u
#define SYS_OPEN_MODE_APPEND 0x10u

#define SYS_FD_FLAG_NONBLOCK 0x00000800u

#define SYS_FCNTL_GETFL 3
#define SYS_FCNTL_SETFL 4

enum {
    SYS_WRITE     = 1,
    SYS_EXIT      = 2,
    SYS_GET_TICKS = 3,
    SYS_PUTC      = 4,
    SYS_READ      = 5,
    SYS_YIELD     = 6,
    SYS_EXEC      = 7,
    SYS_OPEN      = 8,   /* open a FAT16 file; returns fd or -1 */
    SYS_CLOSE     = 9,   /* close an fd */
    SYS_FREAD     = 10,  /* read bytes from an open fd into a user buffer */
    SYS_SLEEP     = 11,  /* block for N timer ticks */
    SYS_WRITEFILE = 12,  /* create/overwrite a root-directory file */
    SYS_HALT      = 13,  /* halt the machine */
    SYS_REBOOT    = 14,  /* reboot the machine */
    SYS_WRITEFILE_PATH = 15, /* create/overwrite a FAT16 file at any path */
    SYS_BRK       = 16,  /* query or grow the calling process heap break */
    SYS_OPEN_WRITE = 17, /* open a FAT16 file for write/truncate */
    SYS_WRITEFD    = 18,  /* write bytes to an open fd */
    SYS_LSEEK      = 19,  /* reposition an open fd */
    SYS_UNLINK     = 20,  /* remove a FAT16 file */
    SYS_RENAME     = 21,  /* rename or move a FAT16 entry */
    SYS_STAT       = 22,  /* query size / directory status for a FAT16 path */

    /*
     * Socket ABI.
     *
     * Intentionally tiny: stream sockets only, IPv4 only, and a minimal poll
     * surface for blocking server loops.
     */
    SYS_SOCKET     = 23,
    SYS_BIND       = 24,
    SYS_LISTEN     = 25,
    SYS_ACCEPT     = 26,
    SYS_CONNECT    = 27,
    SYS_SEND       = 28,
    SYS_RECV       = 29,
    SYS_POLL       = 30,

    SYS_MKDIR      = 31,
    SYS_RMDIR      = 32,
    SYS_DIRLIST    = 33,
    SYS_SETSOCKOPT = 34,
    SYS_GETSOCKNAME = 35,
    SYS_OPEN_MODE   = 36,  /* mode-aware open; ebx=path ecx=SYS_OPEN_MODE_* */
    SYS_GETCWD      = 37,  /* copy process cwd into user buffer */
    SYS_CHDIR       = 38,  /* change process cwd */
    SYS_FSYNC       = 39,  /* flush writable fd data */
    SYS_READ_RAW    = 40,  /* read console input without echo */

    SYS_FCNTL       = 41,  /* descriptor flag operations */
    SYS_EPOLL_CREATE = 42,
    SYS_EPOLL_CTL    = 43,
    SYS_EPOLL_WAIT   = 44,
    SYS_TIMERFD_CREATE = 45,
    SYS_TIMERFD_SETTIME = 46,
    SYS_SIGNALFD       = 47,
    SYS_ACCEPT4        = 48,
    SYS_SHUTDOWN       = 49,
    SYS_GETPEERNAME    = 50,
    SYS_FSTAT          = 51,
    SYS_TERMINAL_SIZE  = 52   /* write active terminal rows/cols */
};

#endif
