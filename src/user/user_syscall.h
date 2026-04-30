#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include "uapi_syscall.h"

typedef unsigned int uint32_t;

/*
 * Raw syscall helpers for SmallOS syscall ABI v1.
 *
 * Register convention:
 *   eax = syscall number
 *   ebx = arg1
 *   ecx = arg2
 *   edx = arg3
 *
 * Return:
 *   eax = result
 *
 * Error convention:
 *   negative value indicates error
 */

static inline int syscall0(int num) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num)
        : "memory"
    );
    return ret;
}

static inline int syscall1(int num, uint32_t arg1) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1)
        : "memory"
    );
    return ret;
}

static inline int syscall2(int num, uint32_t arg1, uint32_t arg2) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2)
        : "memory"
    );
    return ret;
}

static inline int syscall3(int num, uint32_t arg1, uint32_t arg2, uint32_t arg3) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3)
        : "memory"
    );
    return ret;
}

static inline int sys_write(const char* buf, uint32_t len) {
    return syscall2(SYS_WRITE, (uint32_t)buf, len);
}

static inline int sys_putc(char c) {
    return syscall1(SYS_PUTC, (uint32_t)(unsigned char)c);
}

static inline void sys_exit(int code) {
    (void)syscall1(SYS_EXIT, (uint32_t)code);
}

static inline uint32_t sys_get_ticks(void) {
    return (uint32_t)syscall0(SYS_GET_TICKS);
}

static inline int sys_read(char* buf, uint32_t len) {
    return syscall2(SYS_READ, (uint32_t)buf, len);
}

static inline int sys_yield(void) {
    return syscall0(SYS_YIELD);
}

/*
 * sys_sleep(ticks)
 *
 * Block the current process for at least ticks timer ticks.
 */
static inline int sys_sleep(uint32_t ticks) {
    return syscall1(SYS_SLEEP, ticks);
}

/*
 * sys_exec(name, argc, argv)
 *
 * Load and spawn the named ELF from the FAT16 partition.
 *
 * The kernel returns immediately after enqueueing the child.  The child
 * runs independently under the scheduler.
 *
 * name and argv point into the caller's user address space — the kernel
 * reads them while the caller's CR3 is still active.
 *
 * Returns 0 on success, -1 if the program was not found or could not be
 * loaded.
 */
static inline int sys_exec(const char* name, int argc, char** argv) {
    return syscall3(SYS_EXEC, (uint32_t)name, (uint32_t)argc, (uint32_t)argv);
}

/*
 * sys_open(name)
 *
 * Open a file from the FAT16 partition by name.  The name is matched
 * case-insensitively using 8.3 rules (e.g. "readme.txt", "DATA.BIN").
 *
 * Returns a file descriptor (>= 3) on success, or -1 on failure
 * (file not found, fd table full, name too long).
 */
static inline int sys_open(const char* name) {
    return syscall1(SYS_OPEN, (uint32_t)name);
}

/*
 * sys_close(fd)
 *
 * Close an open file descriptor.  Returns 0 on success, -1 on error.
 */
static inline int sys_close(int fd) {
    return syscall1(SYS_CLOSE, (uint32_t)fd);
}

/*
 * sys_fread(fd, buf, len)
 *
 * Read up to len bytes from the file at fd into buf, starting at the
 * current file position.  Advances the position by the number of bytes
 * actually read.
 *
 * Returns the number of bytes read (0 at end-of-file), or -1 on error
 * (bad fd, invalid buffer, read failure).
 */
static inline int sys_fread(int fd, char* buf, uint32_t len) {
    return syscall3(SYS_FREAD, (uint32_t)fd, (uint32_t)buf, len);
}

/*
 * sys_writefile(name, buf, len)
 *
 * Create or overwrite a root-directory FAT16 file with the provided
 * data.  This is the simplest persistence primitive for generated
 * compiler output and similar build artifacts.
 */
static inline int sys_writefile(const char* name, const char* buf, uint32_t len) {
    return syscall3(SYS_WRITEFILE, (uint32_t)name, (uint32_t)buf, len);
}

/*
 * sys_writefile_path(path, buf, len)
 *
 * Create or overwrite a FAT16 file at an arbitrary path.  This is the
 * preferred write primitive for compilers and build tools because it can
 * emit directly into nested directories.
 */
static inline int sys_writefile_path(const char* path, const char* buf, uint32_t len) {
    return syscall3(SYS_WRITEFILE_PATH, (uint32_t)path, (uint32_t)buf, len);
}

static inline int sys_halt(void) {
    return syscall0(SYS_HALT);
}

static inline int sys_reboot(void) {
    return syscall0(SYS_REBOOT);
}

#endif
