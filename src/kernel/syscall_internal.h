#ifndef SYSCALL_INTERNAL_H
#define SYSCALL_INTERNAL_H

#include "types.h"
#include "syscall.h"
#include "uapi_poll.h"
#include "uapi_dirent.h"
#include "uapi_socket.h"
#include "uapi_epoll.h"
#include "uapi_display.h"
#include "uapi_input.h"
#include "uapi_sound.h"
#include "uapi_syscall.h"

typedef struct process process_t;
typedef struct fd_entry fd_entry_t;

#define EXEC_NAME_MAX         128u
#define SYS_AT_FDCWD          (-100)
#define SYS_PERM_X            1u
#define SYS_PERM_W            2u
#define SYS_PERM_R            4u

struct user_itimerspec {
    struct {
        unsigned int tv_sec;
        long tv_nsec;
    } it_interval;
    struct {
        unsigned int tv_sec;
        long tv_nsec;
    } it_value;
};

struct user_timespec {
    unsigned int tv_sec;
    long tv_nsec;
};

int path_is_sep(char c);
int path_build_from(const char* base, const char* path, char* out, unsigned int out_size);
int user_page_mapped(u32* pd, unsigned int addr);
int user_buf_ok(unsigned int ptr, unsigned int len);
int user_buf_write_ok(unsigned int ptr, unsigned int len);
int user_count_bytes_ok(unsigned int ptr, unsigned int count, unsigned int elem_size, unsigned int* out_bytes);
int copy_from_user(void* dst, const void* src, unsigned int len);
int copy_to_user(void* dst, const void* src, unsigned int len);
int read_user_u32(unsigned int* out, const unsigned int* src);
int write_user_u32(unsigned int* dst, unsigned int value);
int copy_user_cstr(char* dst, unsigned int dst_size, const char* src);
int copy_user_path_resolved(char* dst, unsigned int dst_size, const char* src);
int copy_user_path_at_resolved(char* dst, unsigned int dst_size, int dirfd, const char* src);
int path_lookup_errno(const char* path);
int process_is_root(process_t* proc);
int check_path_permission(process_t* proc, const char* path, unsigned int need, sys_stat_info_t* out_info);
unsigned int process_ram_bytes(process_t* proc);
int sys_display_info_impl(sys_display_info_t* out_info);
int sys_display_acquire_impl(void);
int sys_display_release_impl(void);
int sys_display_fill_impl(const sys_display_fill_rect_t* user_req);
int sys_display_blit_impl(const sys_display_blit_rect_t* user_req);
int sys_display_blit_stride_impl(const sys_display_blit_stride_rect_t* user_req);
int sys_display_map_impl(sys_display_map_info_t* out_info);
int sys_display_present_page_impl(sys_display_present_page_t* user_req);
int sys_mouse_read_impl(sys_mouse_state_t* out_state);
int sys_mouse_debug_impl(sys_mousedebug_t* out_info);
int sys_usbinfo_impl(sys_usbinfo_t* out_info);
int sys_usb_diag_op_impl(unsigned int op, unsigned int arg);
int sys_usb_mouse_op_impl(unsigned int op, unsigned int port);
int sys_input_read_impl(syscall_regs_t* regs, sys_input_event_t* out_events, unsigned int max_events, unsigned int flags);
int sys_input_wait_until_impl(syscall_regs_t* regs, unsigned int deadline);
int sys_sound_op_impl(unsigned int op, unsigned int arg1, unsigned int arg2);
int sys_write_impl(const char* buf, unsigned int len);
int sys_putc_impl(unsigned int ch);
int sys_read_impl(char* buf, unsigned int len);
int sys_read_raw_impl(char* buf, unsigned int len);
int sys_fread_impl(int fd, char* buf, unsigned int len);
int sys_dup_impl(int oldfd);
int sys_dup2_impl(int oldfd, int newfd);
int sys_dup3_impl(int oldfd, int newfd, unsigned int flags);
int sys_pipe2_impl(int* user_fds, unsigned int flags);
int sys_pty_open_impl(int* user_fds, unsigned int master_flags);
int sys_pty_set_size_impl(int fd, unsigned int rows, unsigned int cols);
int sys_poll_impl(syscall_regs_t* regs, struct pollfd* fds, unsigned int nfds, int timeout);
int sys_fcntl_impl(int fd, int cmd, unsigned int arg);
int sys_epoll_create_impl(int flags);
int sys_epoll_ctl_impl(int epfd, int op, int fd, struct epoll_event* user_event);
int sys_epoll_wait_impl(syscall_regs_t* regs, int epfd, struct epoll_event* events, int maxevents, int timeout);
int sys_timerfd_create_impl(int clock_id, int flags);
int sys_timerfd_settime_impl(int fd, int flags, const struct user_itimerspec* new_value, struct user_itimerspec* old_value);
int sys_signalfd_impl(int fd, const void* mask, int flags);
int sys_terminal_size_impl(unsigned int* out_rows, unsigned int* out_cols);
int sys_tcgetattr_impl(int fd, sys_termios_t* out);
int sys_tcsetattr_impl(int fd, const sys_termios_t* in);
int sys_tty_ioctl_impl(int fd, unsigned int request, void* arg);
int sys_writefile_impl(const char* name, const void* buf, unsigned int len);
int sys_writefile_path_impl(const char* path, const void* buf, unsigned int len);
int sys_open_impl(const char* name);
int sys_close_impl(int fd);
int sys_open_write_impl(const char* name);
int sys_open_mode_create_impl(const char* name, unsigned int mode, unsigned int create_mode);
int sys_openat_mode_create_impl(int dirfd, const char* name, unsigned int mode, unsigned int create_mode);
int sys_open_mode_impl(const char* name, unsigned int mode);
int sys_mkdir_impl(const char* path, unsigned int mode);
int sys_mkdirat_impl(int dirfd, const char* path, unsigned int mode);
int sys_rmdir_impl(const char* path);
int sys_dirlist_impl(const char* path, unsigned int index, uapi_dirent_t* out);
int sys_dirlist_batch_impl(const char* path, unsigned int index, uapi_dirent_t* out, unsigned int max_count);
int sys_writefd_impl(int fd, const char* buf, unsigned int len);
int sys_lseek_impl(int fd, int offset, int whence);
int sys_fsync_impl(int fd);
int sys_unlink_impl(const char* path);
int sys_unlinkat_impl(int dirfd, const char* path, unsigned int flags);
int sys_link_impl(const char* oldpath, const char* newpath);
int sys_linkat_impl(int olddirfd, const char* oldpath, int newdirfd, const char* newpath, unsigned int flags);
int sys_symlink_impl(const char* target, const char* linkpath);
int sys_symlinkat_impl(const char* target, int newdirfd, const char* linkpath);
int sys_readlink_impl(const char* path, char* out, unsigned int out_size);
int sys_readlinkat_impl(int dirfd, const char* path, char* out, unsigned int out_size);
int sys_rename_impl(const char* src, const char* dst);
int sys_renameat_impl(int olddirfd, const char* oldpath, int newdirfd, const char* newpath);
int sys_chmod_impl(const char* path, unsigned int mode);
int sys_chown_impl(const char* path, unsigned int uid, unsigned int gid);
int sys_utimens_impl(const char* path, const struct user_timespec* times);
int sys_utimensat_impl(int dirfd, const char* path, const struct user_timespec* times, unsigned int flags);
int sys_mknod_impl(const char* path, unsigned int mode, unsigned int dev);
int sys_ftruncate_impl(int fd, unsigned int size);
int sys_fchmod_impl(int fd, unsigned int mode);
int sys_fchown_impl(int fd, unsigned int uid, unsigned int gid);
int sys_futimens_impl(int fd, const struct user_timespec* times);
int sys_statfs_impl(const char* path, sys_statfs_t* out);
int sys_fstatfs_impl(int fd, sys_statfs_t* out);
int sys_mount_impl(const char* source, const char* target, const char* fstype, unsigned int flags, const void* data);
int sys_umount2_impl(const char* target, unsigned int flags);
int sys_fsinfo_impl(sys_fsinfo_t* out_info);
int sys_fsmap_impl(sys_fsmap_request_t* user_req);
int sys_block_read_sector_impl(unsigned int lba, void* user_buf);
int sys_getcwd_impl(char* buf, unsigned int size);
int sys_chdir_impl(const char* path);
int sys_fchdir_impl(int fd);
int sys_stat_impl(const char* path, unsigned int* out_size, int* out_is_dir);
int sys_fstat_impl(int fd, unsigned int* out_size, int* out_is_dir);
int sys_stat_full_impl(const char* path, sys_stat_info_t* out);
int sys_lstat_full_impl(const char* path, sys_stat_info_t* out);
int sys_fstatat_full_impl(int dirfd, const char* path, sys_stat_info_t* out, unsigned int flags);
int sys_fstat_full_impl(int fd, sys_stat_info_t* out);
int sys_socket_impl(int domain, int type, int protocol);
int sys_bind_impl(int fd, const struct sockaddr* addr, unsigned int addrlen);
int sys_listen_impl(int fd, int backlog);
int sys_accept_impl(syscall_regs_t* regs, int fd, struct sockaddr* addr, unsigned int* addrlen, unsigned int flags);
int sys_connect_impl(int fd, const struct sockaddr* addr, unsigned int addrlen);
int sys_send_impl(int fd, const void* buf, unsigned int len);
int sys_netlink_send_user(fd_entry_t* ent, const void* buf, unsigned int len);
int sys_recv_impl(syscall_regs_t* regs, int fd, void* buf, unsigned int len);
int sys_sendto_impl(int fd, const void* buf, unsigned int len, unsigned int flags, const struct sockaddr* dest_addr, unsigned int addrlen);
int sys_recvfrom_impl(syscall_regs_t* regs, int fd, void* buf, unsigned int len, unsigned int flags, struct sockaddr* src_addr, unsigned int* addrlen);
int sys_shutdown_impl(int fd, int how);
int sys_getpeername_impl(int fd, struct sockaddr* addr, unsigned int* addrlen);
int sys_setsockopt_impl(int fd, int level, int optname);
int sys_getsockname_impl(int fd, struct sockaddr* addr, unsigned int* addrlen);
int sys_net_ioctl_impl(int fd, unsigned int request, void* argp);
int sys_netinfo_impl(sys_netinfo_t* out_info);
int sys_net_op_impl(sys_net_op_request_t* user_req);
void sys_exit_impl(syscall_regs_t* regs);
int sys_exec_impl(const char* name, int argc, char** argv);
int sys_exec_fg_impl(const char* name, int argc, char** argv);
int sys_fork_impl(syscall_regs_t* regs);
int sys_execve_impl(syscall_regs_t* regs, const char* name, char** argv, char** envp);
int sys_getpid_impl(void);
int sys_setsid_impl(void);
int sys_getsid_impl(int pid);
int sys_setpgid_impl(int pid, int pgid);
int sys_getpgid_impl(int pid);
int sys_waitpid_impl(int pid, int* user_status, int options);
int sys_waitpid_fg_impl(int pid, int* user_status);
int sys_kill_impl(syscall_regs_t* regs, int pid, int signum);
unsigned int sys_brk_impl(unsigned int new_brk);
int sys_mmap_impl(unsigned int addr, unsigned int length, unsigned int prot, unsigned int flags, int fd, unsigned int offset);
int sys_munmap_impl(unsigned int addr, unsigned int length);
int sys_mprotect_impl(unsigned int addr, unsigned int length, unsigned int prot);
int sys_sigaction_impl(int signum, const sys_sigaction_t* act, sys_sigaction_t* oldact);
int sys_sigprocmask_impl(int how, const unsigned int* set, unsigned int* oldset);
int sys_sigreturn_impl(syscall_regs_t* regs, const sys_signal_frame_t* frame);
unsigned int sys_getuid_impl(void);
unsigned int sys_geteuid_impl(void);
unsigned int sys_getgid_impl(void);
unsigned int sys_getegid_impl(void);
int sys_setuid_impl(unsigned int uid);
int sys_setgid_impl(unsigned int gid);
unsigned int sys_umask_impl(unsigned int mask);
int sys_getrlimit_impl(int resource, sys_rlimit_t* out);
int sys_setrlimit_impl(int resource, const sys_rlimit_t* in);
int sys_getrusage_impl(int who, sys_rusage_t* out);
int sys_meminfo_impl(sys_meminfo_t* out_info);
int sys_procinfo_impl(sys_procinfo_t* out_info);
int sys_e820_entry_impl(unsigned int index, sys_e820_entry_t* out_entry);
unsigned int sys_get_ticks_impl(void);
void sys_wait_until_current_running(process_t* proc);
int sys_yield_impl(void);
int sys_sleep_impl(syscall_regs_t* regs, unsigned int ticks);
int sys_clock_gettime_impl(int clock_id, struct user_timespec* ts);
int sys_clock_settime_impl(int clock_id, const struct user_timespec* ts);
int sys_ntp_sync_impl(unsigned int server_ip, struct user_timespec* out_ts);

#endif /* SYSCALL_INTERNAL_H */
