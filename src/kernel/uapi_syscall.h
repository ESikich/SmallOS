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
    SYS_STAT       = 22   /* query size / directory status for a FAT16 path */
};

#endif
