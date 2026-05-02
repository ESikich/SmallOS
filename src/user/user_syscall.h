#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include "uapi_syscall.h"
#include "uapi_time.h"
#include "uapi_socket.h"
#include "uapi_poll.h"

struct dirent;

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
 * sys_open_write(name)
 *
 * Open a FAT16 file for buffered write/truncate.
 */
static inline int sys_open_write(const char* name) {
    return syscall1(SYS_OPEN_WRITE, (uint32_t)name);
}

/*
 * sys_open_mode(name, mode)
 *
 * Mode-aware descriptor open.  mode is a SYS_OPEN_MODE_* bitmask.
 */
static inline int sys_open_mode(const char* name, uint32_t mode) {
    return syscall2(SYS_OPEN_MODE, (uint32_t)name, mode);
}

static inline int sys_getcwd(char* buf, uint32_t size) {
    return syscall2(SYS_GETCWD, (uint32_t)buf, size);
}

static inline int sys_chdir(const char* path) {
    return syscall1(SYS_CHDIR, (uint32_t)path);
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
 * sys_writefd(fd, buf, len)
 *
 * Write bytes to an open writable file descriptor.
 */
static inline int sys_writefd(int fd, const char* buf, uint32_t len) {
    return syscall3(SYS_WRITEFD, (uint32_t)fd, (uint32_t)buf, len);
}

/*
 * sys_lseek(fd, offset, whence)
 *
 * Reposition an open file descriptor.
 */
static inline int sys_lseek(int fd, int offset, int whence) {
    return syscall3(SYS_LSEEK, (uint32_t)fd, (uint32_t)offset, (uint32_t)whence);
}

/*
 * sys_unlink(path)
 *
 * Remove a FAT16 file.
 */
static inline int sys_unlink(const char* path) {
    return syscall1(SYS_UNLINK, (uint32_t)path);
}

/*
 * sys_rename(src, dst)
 *
 * Rename or move a FAT16 entry.
 */
static inline int sys_rename(const char* src, const char* dst) {
    return syscall2(SYS_RENAME, (uint32_t)src, (uint32_t)dst);
}

/*
 * sys_stat(path, out_size, out_is_dir)
 *
 * Query whether a path exists and whether it resolves to a directory.
 * If the path is a regular file, *out_size receives its size.
 * If the path is a directory, *out_is_dir receives 1 and *out_size is 0.
 */
static inline int sys_stat(const char* path, uint32_t* out_size, int* out_is_dir) {
    return syscall3(SYS_STAT, (uint32_t)path, (uint32_t)out_size, (uint32_t)out_is_dir);
}

static inline int sys_socket(int domain, int type, int protocol) {
    return syscall3(SYS_SOCKET, (uint32_t)domain, (uint32_t)type, (uint32_t)protocol);
}

static inline int sys_bind(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    return syscall3(SYS_BIND, (uint32_t)fd, (uint32_t)addr, (uint32_t)addrlen);
}

static inline int sys_listen(int fd, int backlog) {
    return syscall2(SYS_LISTEN, (uint32_t)fd, (uint32_t)backlog);
}

static inline int sys_accept(int fd, struct sockaddr* addr, socklen_t* addrlen) {
    return syscall3(SYS_ACCEPT, (uint32_t)fd, (uint32_t)addr, (uint32_t)addrlen);
}

static inline int sys_connect(int fd, const struct sockaddr* addr, socklen_t addrlen) {
    return syscall3(SYS_CONNECT, (uint32_t)fd, (uint32_t)addr, (uint32_t)addrlen);
}

static inline int sys_send(int fd, const void* buf, uint32_t len) {
    return syscall3(SYS_SEND, (uint32_t)fd, (uint32_t)buf, len);
}

static inline int sys_recv(int fd, void* buf, uint32_t len) {
    return syscall3(SYS_RECV, (uint32_t)fd, (uint32_t)buf, len);
}

static inline int sys_poll(struct pollfd* fds, nfds_t nfds, int timeout) {
    return syscall3(SYS_POLL, (uint32_t)fds, (uint32_t)nfds, (uint32_t)timeout);
}

static inline int sys_mkdir(const char* path, uint32_t mode) {
    return syscall2(SYS_MKDIR, (uint32_t)path, mode);
}

static inline int sys_rmdir(const char* path) {
    return syscall1(SYS_RMDIR, (uint32_t)path);
}

static inline int sys_dirlist(const char* path,
                              uint32_t index,
                              struct dirent* out) {
    return syscall3(SYS_DIRLIST, (uint32_t)path, index, (uint32_t)out);
}

static inline int sys_setsockopt(int fd, int level, int optname,
                                 const void* optval, socklen_t optlen) {
    (void)optval;
    (void)optlen;
    return syscall3(SYS_SETSOCKOPT, (uint32_t)fd, (uint32_t)level,
                    (uint32_t)optname);
}

static inline int sys_getsockname(int fd, struct sockaddr* addr, socklen_t* addrlen) {
    return syscall3(SYS_GETSOCKNAME, (uint32_t)fd, (uint32_t)addr, (uint32_t)addrlen);
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

/*
 * sys_brk(new_brk)
 *
 * Query or adjust the current process heap break.  Passing 0 returns the
 * current break.  The kernel may grow or shrink the heap on page
 * boundaries.
 */
static inline uint32_t sys_brk(uint32_t new_brk) {
    return (uint32_t)syscall1(SYS_BRK, new_brk);
}

static inline int sys_halt(void) {
    return syscall0(SYS_HALT);
}

static inline int sys_reboot(void) {
    return syscall0(SYS_REBOOT);
}

#endif
