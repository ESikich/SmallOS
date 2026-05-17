#ifndef USER_SYSCALL_H
#define USER_SYSCALL_H

#include "uapi_syscall.h"
#include "uapi_time.h"
#include "uapi_socket.h"
#include "uapi_poll.h"
#include "uapi_epoll.h"
#include "uapi_display.h"
#include "uapi_input.h"

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
 *   esi = arg4 for four-argument calls
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

static inline int syscall4(int num, uint32_t arg1, uint32_t arg2, uint32_t arg3, uint32_t arg4) {
    int ret;
    __asm__ volatile (
        "int $0x80"
        : "=a"(ret)
        : "a"(num), "b"(arg1), "c"(arg2), "d"(arg3), "S"(arg4)
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

static inline int sys_read_raw(char* buf, uint32_t len) {
    return syscall2(SYS_READ_RAW, (uint32_t)buf, len);
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
 * Load and spawn the named ELF from the ext2 partition.
 *
 * The kernel returns immediately after enqueueing the child.  The child
 * runs independently under the scheduler.
 *
 * name and argv point into the caller's user address space — the kernel
 * reads them while the caller's CR3 is still active.
 *
 * Returns the child pid on success or a negative errno if the program was not found,
 * could not be loaded, or failed validation.
 */
static inline int sys_exec(const char* name, int argc, char** argv) {
    return syscall3(SYS_EXEC, (uint32_t)name, (uint32_t)argc, (uint32_t)argv);
}

static inline int sys_exec_foreground(const char* name, int argc, char** argv) {
    return syscall3(SYS_EXEC_FG, (uint32_t)name, (uint32_t)argc, (uint32_t)argv);
}

static inline int sys_getpid(void) {
    return syscall0(SYS_GETPID);
}

static inline int sys_waitpid(int pid, int* status, int options) {
    return syscall3(SYS_WAITPID, (uint32_t)pid, (uint32_t)status, (uint32_t)options);
}

static inline int sys_waitpid_foreground(int pid, int* status) {
    return syscall2(SYS_WAITPID_FG, (uint32_t)pid, (uint32_t)status);
}

static inline int sys_kill(int pid, int signum) {
    return syscall2(SYS_KILL, (uint32_t)pid, (uint32_t)signum);
}

/*
 * sys_open(name)
 *
 * Open a file from the ext2 partition by path.  Each component is matched
 * with native case-sensitive ext2 names (e.g. "usr/bin/hello.elf").
 *
 * Returns a file descriptor (>= 3) on success, or a negative errno on
 * failure (file not found, fd table full, path too long, invalid pointer).
 */
static inline int sys_open(const char* name) {
    return syscall1(SYS_OPEN, (uint32_t)name);
}

/*
 * sys_open_write(name)
 *
 * Open an ext2 file for streaming write/truncate.
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
 * Close an open file descriptor.  Returns 0 on success or a negative errno
 * on error.
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
 * Returns the number of bytes read (0 at end-of-file), or a negative errno
 * on error (bad fd, invalid buffer, read failure).
 */
static inline int sys_fread(int fd, char* buf, uint32_t len) {
    return syscall3(SYS_FREAD, (uint32_t)fd, (uint32_t)buf, len);
}

/*
 * sys_writefd(fd, buf, len)
 *
 * Write bytes to an open writable file descriptor.  Returns the byte count
 * or a negative errno.
 */
static inline int sys_writefd(int fd, const char* buf, uint32_t len) {
    return syscall3(SYS_WRITEFD, (uint32_t)fd, (uint32_t)buf, len);
}

/*
 * sys_lseek(fd, offset, whence)
 *
 * Reposition an open file descriptor.  Returns the new offset or a negative
 * errno.
 */
static inline int sys_lseek(int fd, int offset, int whence) {
    return syscall3(SYS_LSEEK, (uint32_t)fd, (uint32_t)offset, (uint32_t)whence);
}

static inline int sys_fsync(int fd) {
    return syscall1(SYS_FSYNC, (uint32_t)fd);
}

/*
 * sys_unlink(path)
 *
 * Remove an ext2 file.
 */
static inline int sys_unlink(const char* path) {
    return syscall1(SYS_UNLINK, (uint32_t)path);
}

/*
 * sys_rename(src, dst)
 *
 * Rename or move an ext2 entry.
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

static inline int sys_fstat(int fd, uint32_t* out_size, int* out_is_dir) {
    return syscall3(SYS_FSTAT, (uint32_t)fd, (uint32_t)out_size, (uint32_t)out_is_dir);
}

static inline int sys_stat_full(const char* path, sys_stat_info_t* out) {
    return syscall2(SYS_STAT_FULL, (uint32_t)path, (uint32_t)out);
}

static inline int sys_fstat_full(int fd, sys_stat_info_t* out) {
    return syscall2(SYS_FSTAT_FULL, (uint32_t)fd, (uint32_t)out);
}

static inline int sys_fsinfo(sys_fsinfo_t* out_info) {
    return syscall1(SYS_FSINFO, (uint32_t)out_info);
}

static inline int sys_fsmap(sys_fsmap_request_t* req) {
    return syscall1(SYS_FSMAP, (uint32_t)req);
}

static inline int sys_meminfo(sys_meminfo_t* out_info) {
    return syscall1(SYS_MEMINFO, (uint32_t)out_info);
}

static inline int sys_procinfo(sys_procinfo_t* out_info) {
    return syscall1(SYS_PROCINFO, (uint32_t)out_info);
}

static inline int sys_e820_entry(uint32_t index, sys_e820_entry_t* out_entry) {
    return syscall2(SYS_E820_ENTRY, index, (uint32_t)out_entry);
}

static inline int sys_netinfo(sys_netinfo_t* out_info) {
    return syscall1(SYS_NETINFO, (uint32_t)out_info);
}

static inline int sys_net_op(sys_net_op_request_t* req) {
    return syscall1(SYS_NET_OP, (uint32_t)req);
}

static inline int sys_block_read_sector(uint32_t lba, void* buf) {
    return syscall2(SYS_BLOCK_READ_SECTOR, lba, (uint32_t)buf);
}

static inline int sys_ata_read_sector(uint32_t lba, void* buf) {
    return sys_block_read_sector(lba, buf);
}

static inline int sys_terminal_size(uint32_t* out_rows, uint32_t* out_cols) {
    return syscall2(SYS_TERMINAL_SIZE, (uint32_t)out_rows, (uint32_t)out_cols);
}

static inline int sys_display_info(sys_display_info_t* out_info) {
    return syscall1(SYS_DISPLAY_INFO, (uint32_t)out_info);
}

static inline int sys_display_acquire(void) {
    return syscall0(SYS_DISPLAY_ACQUIRE);
}

static inline int sys_display_release(void) {
    return syscall0(SYS_DISPLAY_RELEASE);
}

static inline int sys_display_fill(uint32_t x, uint32_t y, uint32_t w,
                                   uint32_t h, uint32_t color) {
    sys_display_fill_rect_t req;
    req.x = x;
    req.y = y;
    req.w = w;
    req.h = h;
    req.color = color;
    return syscall1(SYS_DISPLAY_FILL, (uint32_t)&req);
}

static inline int sys_display_blit(uint32_t x, uint32_t y, uint32_t w,
                                   uint32_t h, const uint32_t* pixels) {
    sys_display_blit_rect_t req;
    req.x = x;
    req.y = y;
    req.w = w;
    req.h = h;
    req.pixels = pixels;
    return syscall1(SYS_DISPLAY_BLIT, (uint32_t)&req);
}

static inline int sys_mouse_read(sys_mouse_state_t* out_state) {
    return syscall1(SYS_MOUSE_READ, (uint32_t)out_state);
}

static inline int sys_usb_mouse_op(uint32_t op, uint32_t port) {
    return syscall2(SYS_USB_MOUSE_OP, op, port);
}

static inline int sys_usbinfo(sys_usbinfo_t* out_info) {
    return syscall1(SYS_USBINFO, (uint32_t)out_info);
}

static inline int sys_mouse_debug(sys_mousedebug_t* out_info) {
    return syscall1(SYS_MOUSE_DEBUG, (uint32_t)out_info);
}

static inline int sys_usb_diag_op(uint32_t op, uint32_t arg) {
    return syscall2(SYS_USB_DIAG_OP, op, arg);
}

static inline int sys_usb_port_snapshot(sys_usb_port_snapshot_t* out) {
    return syscall2(SYS_USB_DIAG_OP,
                    SYS_USB_DIAG_OP_PORT_SNAPSHOT,
                    (uint32_t)out);
}

static inline int sys_input_read(sys_input_event_t* out_events,
                                 uint32_t max_events,
                                 uint32_t flags) {
    return syscall3(SYS_INPUT_READ, (uint32_t)out_events, max_events, flags);
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

static inline int sys_accept4(int fd, struct sockaddr* addr, socklen_t* addrlen, int flags) {
    return syscall4(SYS_ACCEPT4, (uint32_t)fd, (uint32_t)addr, (uint32_t)addrlen, (uint32_t)flags);
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

static inline int sys_dirlist_batch(const char* path,
                                    uint32_t index,
                                    struct dirent* out,
                                    uint32_t max_count) {
    return syscall4(SYS_DIRLIST_BATCH, (uint32_t)path, index,
                    (uint32_t)out, max_count);
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

static inline int sys_getpeername(int fd, struct sockaddr* addr, socklen_t* addrlen) {
    return syscall3(SYS_GETPEERNAME, (uint32_t)fd, (uint32_t)addr, (uint32_t)addrlen);
}

static inline int sys_shutdown(int fd, int how) {
    return syscall2(SYS_SHUTDOWN, (uint32_t)fd, (uint32_t)how);
}

static inline int sys_fcntl(int fd, int cmd, uint32_t arg) {
    return syscall3(SYS_FCNTL, (uint32_t)fd, (uint32_t)cmd, arg);
}

static inline int sys_pipe(int fds[2]) {
    return syscall1(SYS_PIPE, (uint32_t)fds);
}

static inline int sys_pipe2(int fds[2], int flags) {
    return syscall2(SYS_PIPE2, (uint32_t)fds, (uint32_t)flags);
}

static inline int sys_pty_open(int fds[2], int master_flags) {
    return syscall2(SYS_PTY_OPEN, (uint32_t)fds, (uint32_t)master_flags);
}

static inline int sys_pty_set_size(int fd, uint32_t rows, uint32_t cols) {
    return syscall3(SYS_PTY_SET_SIZE, (uint32_t)fd, rows, cols);
}

static inline int sys_dup(int oldfd) {
    return syscall1(SYS_DUP, (uint32_t)oldfd);
}

static inline int sys_dup2(int oldfd, int newfd) {
    return syscall2(SYS_DUP2, (uint32_t)oldfd, (uint32_t)newfd);
}

static inline int sys_dup3(int oldfd, int newfd, int flags) {
    return syscall3(SYS_DUP3, (uint32_t)oldfd, (uint32_t)newfd, (uint32_t)flags);
}

static inline int sys_fork(void) {
    return syscall0(SYS_FORK);
}

static inline int sys_execve(const char* path, char* const argv[], char* const envp[]) {
    return syscall3(SYS_EXECVE, (uint32_t)path, (uint32_t)argv, (uint32_t)envp);
}

static inline int sys_epoll_create(int flags) {
    return syscall1(SYS_EPOLL_CREATE, (uint32_t)flags);
}

static inline int sys_epoll_ctl(int epfd, int op, int fd, struct epoll_event* event) {
    return syscall4(SYS_EPOLL_CTL, (uint32_t)epfd, (uint32_t)op, (uint32_t)fd, (uint32_t)event);
}

static inline int sys_epoll_wait(int epfd, struct epoll_event* events, int maxevents, int timeout) {
    return syscall4(SYS_EPOLL_WAIT, (uint32_t)epfd, (uint32_t)events, (uint32_t)maxevents, (uint32_t)timeout);
}

static inline int sys_timerfd_create(int clock_id, int flags) {
    return syscall2(SYS_TIMERFD_CREATE, (uint32_t)clock_id, (uint32_t)flags);
}

static inline int sys_timerfd_settime(int fd, int flags, const void* new_value, void* old_value) {
    return syscall4(SYS_TIMERFD_SETTIME, (uint32_t)fd, (uint32_t)flags, (uint32_t)new_value, (uint32_t)old_value);
}

static inline int sys_clock_gettime(int clock_id, void* ts) {
    return syscall2(SYS_CLOCK_GETTIME, (uint32_t)clock_id, (uint32_t)ts);
}

static inline int sys_clock_settime(int clock_id, const void* ts) {
    return syscall2(SYS_CLOCK_SETTIME, (uint32_t)clock_id, (uint32_t)ts);
}

static inline int sys_ntp_sync(uint32_t server_ip, void* out_ts) {
    return syscall2(SYS_NTP_SYNC, server_ip, (uint32_t)out_ts);
}

static inline int sys_signalfd(int fd, const void* mask, int flags) {
    return syscall3(SYS_SIGNALFD, (uint32_t)fd, (uint32_t)mask, (uint32_t)flags);
}

/*
 * sys_writefile(name, buf, len)
 *
 * Create or overwrite a root-directory ext2 file with the provided
 * data.  This is the simplest persistence primitive for generated
 * compiler output and similar build artifacts.
 */
static inline int sys_writefile(const char* name, const char* buf, uint32_t len) {
    return syscall3(SYS_WRITEFILE, (uint32_t)name, (uint32_t)buf, len);
}

/*
 * sys_writefile_path(path, buf, len)
 *
 * Create or overwrite an ext2 file at an arbitrary path.  This is the
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
