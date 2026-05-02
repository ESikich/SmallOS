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
 *
 * Return value:
 *   eax = result
 *
 * Error convention:
 *   negative value means error
 *   -1 is the current generic error return
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
     * The first cut is intentionally tiny: stream sockets only, IPv4 only,
     * and a minimal poll surface for blocking server loops.
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
    SYS_OPEN_MODE   = 36   /* mode-aware open; ebx=path ecx=SYS_OPEN_MODE_* */
};

#endif
