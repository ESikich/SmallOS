#include "syscall_internal.h"
#include "system.h"
#include "uapi_errno.h"

void syscall_handler_main(syscall_regs_t* regs) {
    if (regs == 0) return;

    switch (regs->eax) {
        case SYS_WRITE:
            regs->eax = (unsigned int)sys_write_impl(
                            (const char*)regs->ebx, regs->ecx);
            break;

        case SYS_EXIT:
            sys_exit_impl(regs);
            break;

        case SYS_GET_TICKS:
            regs->eax = sys_get_ticks_impl();
            break;

        case SYS_PUTC:
            regs->eax = (unsigned int)sys_putc_impl(regs->ebx);
            break;

        case SYS_READ:
            regs->eax = (unsigned int)sys_read_impl(
                            (char*)regs->ebx,
                            regs->ecx);
            break;

        case SYS_READ_RAW:
            regs->eax = (unsigned int)sys_read_raw_impl(
                            (char*)regs->ebx,
                            regs->ecx);
            break;

        case SYS_YIELD:
            regs->eax = (unsigned int)sys_yield_impl();
            break;

        case SYS_SLEEP:
            regs->eax = (unsigned int)sys_sleep_impl(regs, regs->ebx);
            break;

        case SYS_WRITEFILE:
            regs->eax = (unsigned int)sys_writefile_impl(
                            (const char*)regs->ebx,
                            (const void*)regs->ecx,
                            regs->edx);
            break;

        case SYS_WRITEFILE_PATH:
            regs->eax = (unsigned int)sys_writefile_path_impl(
                            (const char*)regs->ebx,
                            (const void*)regs->ecx,
                            regs->edx);
            break;

        case SYS_BRK:
            regs->eax = sys_brk_impl(regs->ebx);
            break;

        case SYS_MMAP:
            regs->eax = (unsigned int)sys_mmap_impl(regs->ebx,
                                                    regs->ecx,
                                                    regs->edx,
                                                    regs->esi,
                                                    (int)regs->edi,
                                                    regs->ebp);
            break;

        case SYS_MUNMAP:
            regs->eax = (unsigned int)sys_munmap_impl(regs->ebx,
                                                      regs->ecx);
            break;

        case SYS_MPROTECT:
            regs->eax = (unsigned int)sys_mprotect_impl(regs->ebx,
                                                        regs->ecx,
                                                        regs->edx);
            break;

        case SYS_SIGACTION:
            regs->eax = (unsigned int)sys_sigaction_impl((int)regs->ebx,
                                                         (const sys_sigaction_t*)regs->ecx,
                                                         (sys_sigaction_t*)regs->edx);
            break;

        case SYS_SIGPROCMASK:
            regs->eax = (unsigned int)sys_sigprocmask_impl((int)regs->ebx,
                                                           (const unsigned int*)regs->ecx,
                                                           (unsigned int*)regs->edx);
            break;

        case SYS_SIGRETURN:
        {
            int rc = sys_sigreturn_impl(regs, (const sys_signal_frame_t*)regs->ebx);
            if (rc < 0) regs->eax = (unsigned int)rc;
            break;
        }

        case SYS_HALT:
            system_halt();
            regs->eax = 0;
            break;

        case SYS_REBOOT:
            system_reboot();
            regs->eax = 0;
            break;

        case SYS_EXEC:
            regs->eax = (unsigned int)sys_exec_impl(
                            (const char*)regs->ebx,
                            (int)regs->ecx,
                            (char**)regs->edx);
            break;

        case SYS_EXEC_FG:
            regs->eax = (unsigned int)sys_exec_fg_impl(
                            (const char*)regs->ebx,
                            (int)regs->ecx,
                            (char**)regs->edx);
            break;

        case SYS_GETPID:
            regs->eax = (unsigned int)sys_getpid_impl();
            break;

        case SYS_WAITPID:
            regs->eax = (unsigned int)sys_waitpid_impl(
                            (int)regs->ebx,
                            (int*)regs->ecx,
                            (int)regs->edx);
            break;

        case SYS_WAITPID_FG:
            regs->eax = (unsigned int)sys_waitpid_fg_impl(
                            (int)regs->ebx,
                            (int*)regs->ecx);
            break;

        case SYS_KILL:
            regs->eax = (unsigned int)sys_kill_impl(
                            regs,
                            (int)regs->ebx,
                            (int)regs->ecx);
            break;

        case SYS_OPEN:
            regs->eax = (unsigned int)sys_open_impl(
                            (const char*)regs->ebx);
            break;

        case SYS_OPEN_WRITE:
            regs->eax = (unsigned int)sys_open_write_impl(
                            (const char*)regs->ebx);
            break;

        case SYS_OPEN_MODE:
            regs->eax = (unsigned int)sys_open_mode_impl(
                            (const char*)regs->ebx,
                            (unsigned int)regs->ecx);
            break;

        case SYS_OPEN_CREATE_MODE:
            regs->eax = (unsigned int)sys_open_mode_create_impl(
                            (const char*)regs->ebx,
                            (unsigned int)regs->ecx,
                            (unsigned int)regs->edx);
            break;

        case SYS_OPENAT_CREATE_MODE:
            regs->eax = (unsigned int)sys_openat_mode_create_impl(
                            (int)regs->ebx,
                            (const char*)regs->ecx,
                            (unsigned int)regs->edx,
                            (unsigned int)regs->esi);
            break;

        case SYS_CLOSE:
            regs->eax = (unsigned int)sys_close_impl((int)regs->ebx);
            break;

        case SYS_FREAD:
            regs->eax = (unsigned int)sys_fread_impl(
                            (int)regs->ebx,
                            (char*)regs->ecx,
                            regs->edx);
            break;

        case SYS_WRITEFD:
            regs->eax = (unsigned int)sys_writefd_impl(
                            (int)regs->ebx,
                            (const char*)regs->ecx,
                            regs->edx);
            break;

        case SYS_LSEEK:
            regs->eax = (unsigned int)sys_lseek_impl(
                            (int)regs->ebx,
                            (int)regs->ecx,
                            (int)regs->edx);
            break;

        case SYS_UNLINK:
            regs->eax = (unsigned int)sys_unlink_impl((const char*)regs->ebx);
            break;

        case SYS_RENAME:
            regs->eax = (unsigned int)sys_rename_impl((const char*)regs->ebx,
                                                      (const char*)regs->ecx);
            break;

        case SYS_STAT:
            regs->eax = (unsigned int)sys_stat_impl((const char*)regs->ebx,
                                                    (unsigned int*)regs->ecx,
                                                    (int*)regs->edx);
            break;

        case SYS_SOCKET:
            regs->eax = (unsigned int)sys_socket_impl((int)regs->ebx,
                                                      (int)regs->ecx,
                                                      (int)regs->edx);
            break;

        case SYS_BIND:
            regs->eax = (unsigned int)sys_bind_impl((int)regs->ebx,
                                                    (const struct sockaddr*)regs->ecx,
                                                    regs->edx);
            break;

        case SYS_LISTEN:
            regs->eax = (unsigned int)sys_listen_impl((int)regs->ebx,
                                                      (int)regs->ecx);
            break;

        case SYS_ACCEPT:
        {
            regs->eax = (unsigned int)sys_accept_impl(regs,
                                                      (int)regs->ebx,
                                                      (struct sockaddr*)regs->ecx,
                                                      (unsigned int*)regs->edx,
                                                      0u);
            break;
        }

        case SYS_CONNECT:
            regs->eax = (unsigned int)sys_connect_impl((int)regs->ebx,
                                                       (const struct sockaddr*)regs->ecx,
                                                       regs->edx);
            break;

        case SYS_SEND:
            regs->eax = (unsigned int)sys_send_impl((int)regs->ebx,
                                                    (const void*)regs->ecx,
                                                    regs->edx);
            break;

        case SYS_RECV:
            regs->eax = (unsigned int)sys_recv_impl(regs,
                                                    (int)regs->ebx,
                                                    (void*)regs->ecx,
                                                    regs->edx);
            break;

        case SYS_SENDTO:
            regs->eax = (unsigned int)sys_sendto_impl((int)regs->ebx,
                                                      (const void*)regs->ecx,
                                                      regs->edx,
                                                      regs->esi,
                                                      (const struct sockaddr*)regs->edi,
                                                      regs->ebp);
            break;

        case SYS_RECVFROM:
            regs->eax = (unsigned int)sys_recvfrom_impl(regs,
                                                        (int)regs->ebx,
                                                        (void*)regs->ecx,
                                                        regs->edx,
                                                        regs->esi,
                                                        (struct sockaddr*)regs->edi,
                                                        (unsigned int*)regs->ebp);
            break;

        case SYS_POLL:
            regs->eax = (unsigned int)sys_poll_impl(regs,
                                                    (struct pollfd*)regs->ebx,
                                                    regs->ecx,
                                                    (int)regs->edx);
            break;

        case SYS_MKDIR:
            regs->eax = (unsigned int)sys_mkdir_impl((const char*)regs->ebx,
                                                     regs->ecx);
            break;

        case SYS_MKDIRAT:
            regs->eax = (unsigned int)sys_mkdirat_impl((int)regs->ebx,
                                                       (const char*)regs->ecx,
                                                       regs->edx);
            break;

        case SYS_RMDIR:
            regs->eax = (unsigned int)sys_rmdir_impl((const char*)regs->ebx);
            break;

        case SYS_DIRLIST:
            regs->eax = (unsigned int)sys_dirlist_impl((const char*)regs->ebx,
                                                       regs->ecx,
                                                       (uapi_dirent_t*)regs->edx);
            break;

        case SYS_DIRLIST_BATCH:
            regs->eax = (unsigned int)sys_dirlist_batch_impl((const char*)regs->ebx,
                                                             regs->ecx,
                                                             (uapi_dirent_t*)regs->edx,
                                                             regs->esi);
            break;

        case SYS_SETSOCKOPT:
            regs->eax = (unsigned int)sys_setsockopt_impl((int)regs->ebx,
                                                          (int)regs->ecx,
                                                          (int)regs->edx);
            break;

        case SYS_GETSOCKNAME:
            regs->eax = (unsigned int)sys_getsockname_impl((int)regs->ebx,
                                                           (struct sockaddr*)regs->ecx,
                                                           (unsigned int*)regs->edx);
            break;

        case SYS_GETCWD:
            regs->eax = (unsigned int)sys_getcwd_impl((char*)regs->ebx,
                                                      regs->ecx);
            break;

        case SYS_CHDIR:
            regs->eax = (unsigned int)sys_chdir_impl((const char*)regs->ebx);
            break;

        case SYS_FCHDIR:
            regs->eax = (unsigned int)sys_fchdir_impl((int)regs->ebx);
            break;

        case SYS_FSYNC:
            regs->eax = (unsigned int)sys_fsync_impl((int)regs->ebx);
            break;

        case SYS_FCNTL:
            regs->eax = (unsigned int)sys_fcntl_impl((int)regs->ebx,
                                                     (int)regs->ecx,
                                                     regs->edx);
            break;

        case SYS_PIPE:
            regs->eax = (unsigned int)sys_pipe2_impl((int*)regs->ebx, 0);
            break;

        case SYS_PIPE2:
            regs->eax = (unsigned int)sys_pipe2_impl((int*)regs->ebx, regs->ecx);
            break;

        case SYS_PTY_OPEN:
            regs->eax = (unsigned int)sys_pty_open_impl((int*)regs->ebx, regs->ecx);
            break;

        case SYS_PTY_SET_SIZE:
            regs->eax = (unsigned int)sys_pty_set_size_impl((int)regs->ebx,
                                                            regs->ecx,
                                                            regs->edx);
            break;

        case SYS_TCGETATTR:
            regs->eax = (unsigned int)sys_tcgetattr_impl((int)regs->ebx,
                                                         (sys_termios_t*)regs->ecx);
            break;

        case SYS_TCSETATTR:
            regs->eax = (unsigned int)sys_tcsetattr_impl((int)regs->ebx,
                                                         (const sys_termios_t*)regs->ecx);
            break;

        case SYS_TTY_IOCTL:
            regs->eax = (unsigned int)sys_tty_ioctl_impl((int)regs->ebx,
                                                         regs->ecx,
                                                         (void*)regs->edx);
            break;

        case SYS_GETRLIMIT:
            regs->eax = (unsigned int)sys_getrlimit_impl((int)regs->ebx,
                                                         (sys_rlimit_t*)regs->ecx);
            break;

        case SYS_SETRLIMIT:
            regs->eax = (unsigned int)sys_setrlimit_impl((int)regs->ebx,
                                                         (const sys_rlimit_t*)regs->ecx);
            break;

        case SYS_GETRUSAGE:
            regs->eax = (unsigned int)sys_getrusage_impl((int)regs->ebx,
                                                         (sys_rusage_t*)regs->ecx);
            break;

        case SYS_SETSID:
            regs->eax = (unsigned int)sys_setsid_impl();
            break;

        case SYS_GETSID:
            regs->eax = (unsigned int)sys_getsid_impl((int)regs->ebx);
            break;

        case SYS_SETPGID:
            regs->eax = (unsigned int)sys_setpgid_impl((int)regs->ebx,
                                                       (int)regs->ecx);
            break;

        case SYS_GETPGID:
            regs->eax = (unsigned int)sys_getpgid_impl((int)regs->ebx);
            break;

        case SYS_MOUNT:
            regs->eax = (unsigned int)sys_mount_impl((const char*)regs->ebx,
                                                     (const char*)regs->ecx,
                                                     (const char*)regs->edx,
                                                     regs->esi,
                                                     (const void*)regs->edi);
            break;

        case SYS_UMOUNT2:
            regs->eax = (unsigned int)sys_umount2_impl((const char*)regs->ebx,
                                                       regs->ecx);
            break;

        case SYS_STATFS:
            regs->eax = (unsigned int)sys_statfs_impl((const char*)regs->ebx,
                                                      (sys_statfs_t*)regs->ecx);
            break;

        case SYS_FSTATFS:
            regs->eax = (unsigned int)sys_fstatfs_impl((int)regs->ebx,
                                                       (sys_statfs_t*)regs->ecx);
            break;

        case SYS_STAT_FULL:
            regs->eax = (unsigned int)sys_stat_full_impl((const char*)regs->ebx,
                                                         (sys_stat_info_t*)regs->ecx);
            break;

        case SYS_FSTAT_FULL:
            regs->eax = (unsigned int)sys_fstat_full_impl((int)regs->ebx,
                                                          (sys_stat_info_t*)regs->ecx);
            break;

        case SYS_DUP:
            regs->eax = (unsigned int)sys_dup_impl((int)regs->ebx);
            break;

        case SYS_DUP2:
            regs->eax = (unsigned int)sys_dup2_impl((int)regs->ebx,
                                                    (int)regs->ecx);
            break;

        case SYS_DUP3:
            regs->eax = (unsigned int)sys_dup3_impl((int)regs->ebx,
                                                    (int)regs->ecx,
                                                    regs->edx);
            break;

        case SYS_FORK:
            regs->eax = (unsigned int)sys_fork_impl(regs);
            break;

        case SYS_EXECVE:
            regs->eax = (unsigned int)sys_execve_impl(regs,
                                                      (const char*)regs->ebx,
                                                      (char**)regs->ecx,
                                                      (char**)regs->edx);
            break;

        case SYS_EPOLL_CREATE:
            regs->eax = (unsigned int)sys_epoll_create_impl((int)regs->ebx);
            break;

        case SYS_EPOLL_CTL:
            regs->eax = (unsigned int)sys_epoll_ctl_impl((int)regs->ebx,
                                                         (int)regs->ecx,
                                                         (int)regs->edx,
                                                         (struct epoll_event*)regs->esi);
            break;

        case SYS_EPOLL_WAIT:
            regs->eax = (unsigned int)sys_epoll_wait_impl(regs,
                                                          (int)regs->ebx,
                                                          (struct epoll_event*)regs->ecx,
                                                          (int)regs->edx,
                                                          (int)regs->esi);
            break;

        case SYS_TIMERFD_CREATE:
            regs->eax = (unsigned int)sys_timerfd_create_impl((int)regs->ebx,
                                                              (int)regs->ecx);
            break;

        case SYS_TIMERFD_SETTIME:
            regs->eax = (unsigned int)sys_timerfd_settime_impl(
                            (int)regs->ebx,
                            (int)regs->ecx,
                            (const struct user_itimerspec*)regs->edx,
                            (struct user_itimerspec*)regs->esi);
            break;

        case SYS_SIGNALFD:
            regs->eax = (unsigned int)sys_signalfd_impl((int)regs->ebx,
                                                        (const void*)regs->ecx,
                                                        (int)regs->edx);
            break;

        case SYS_ACCEPT4:
            regs->eax = (unsigned int)sys_accept_impl(regs,
                                                      (int)regs->ebx,
                                                      (struct sockaddr*)regs->ecx,
                                                      (unsigned int*)regs->edx,
                                                      regs->esi);
            break;

        case SYS_SHUTDOWN:
            regs->eax = (unsigned int)sys_shutdown_impl((int)regs->ebx,
                                                       (int)regs->ecx);
            break;

        case SYS_GETPEERNAME:
            regs->eax = (unsigned int)sys_getpeername_impl((int)regs->ebx,
                                                           (struct sockaddr*)regs->ecx,
                                                           (unsigned int*)regs->edx);
            break;

        case SYS_FSTAT:
            regs->eax = (unsigned int)sys_fstat_impl((int)regs->ebx,
                                                     (unsigned int*)regs->ecx,
                                                     (int*)regs->edx);
            break;

        case SYS_TERMINAL_SIZE:
            regs->eax = (unsigned int)sys_terminal_size_impl(
                            (unsigned int*)regs->ebx,
                            (unsigned int*)regs->ecx);
            break;

        case SYS_DISPLAY_INFO:
            regs->eax = (unsigned int)sys_display_info_impl(
                            (sys_display_info_t*)regs->ebx);
            break;

        case SYS_DISPLAY_FILL:
            regs->eax = (unsigned int)sys_display_fill_impl(
                            (const sys_display_fill_rect_t*)regs->ebx);
            break;

        case SYS_DISPLAY_BLIT:
            regs->eax = (unsigned int)sys_display_blit_impl(
                            (const sys_display_blit_rect_t*)regs->ebx);
            break;

        case SYS_DISPLAY_BLIT_STRIDE:
            regs->eax = (unsigned int)sys_display_blit_stride_impl(
                            (const sys_display_blit_stride_rect_t*)regs->ebx);
            break;

        case SYS_DISPLAY_MAP:
            regs->eax = (unsigned int)sys_display_map_impl(
                            (sys_display_map_info_t*)regs->ebx);
            break;

        case SYS_DISPLAY_PRESENT_PAGE:
            regs->eax = (unsigned int)sys_display_present_page_impl(
                            (sys_display_present_page_t*)regs->ebx);
            break;

        case SYS_DISPLAY_ACQUIRE:
            regs->eax = (unsigned int)sys_display_acquire_impl();
            break;

        case SYS_DISPLAY_RELEASE:
            regs->eax = (unsigned int)sys_display_release_impl();
            break;

        case SYS_MOUSE_READ:
            regs->eax = (unsigned int)sys_mouse_read_impl(
                            (sys_mouse_state_t*)regs->ebx);
            break;

        case SYS_USB_MOUSE_OP:
            regs->eax = (unsigned int)sys_usb_mouse_op_impl(
                            regs->ebx,
                            regs->ecx);
            break;

        case SYS_USBINFO:
            regs->eax = (unsigned int)sys_usbinfo_impl(
                            (sys_usbinfo_t*)regs->ebx);
            break;

        case SYS_MOUSE_DEBUG:
            regs->eax = (unsigned int)sys_mouse_debug_impl(
                            (sys_mousedebug_t*)regs->ebx);
            break;

        case SYS_USB_DIAG_OP:
            regs->eax = (unsigned int)sys_usb_diag_op_impl(
                            regs->ebx,
                            regs->ecx);
            break;

        case SYS_INPUT_READ:
            regs->eax = (unsigned int)sys_input_read_impl(
                            regs,
                            (sys_input_event_t*)regs->ebx,
                            regs->ecx,
                            regs->edx);
            break;

        case SYS_INPUT_WAIT_UNTIL:
            regs->eax = (unsigned int)sys_input_wait_until_impl(
                            regs,
                            regs->ebx);
            break;

        case SYS_SOUND_OP:
            regs->eax = (unsigned int)sys_sound_op_impl(
                            regs->ebx,
                            regs->ecx,
                            regs->edx);
            break;

        case SYS_FSINFO:
            regs->eax = (unsigned int)sys_fsinfo_impl(
                            (sys_fsinfo_t*)regs->ebx);
            break;

        case SYS_FSMAP:
            regs->eax = (unsigned int)sys_fsmap_impl(
                            (sys_fsmap_request_t*)regs->ebx);
            break;

        case SYS_MEMINFO:
            regs->eax = (unsigned int)sys_meminfo_impl(
                            (sys_meminfo_t*)regs->ebx);
            break;

        case SYS_PROCINFO:
            regs->eax = (unsigned int)sys_procinfo_impl(
                            (sys_procinfo_t*)regs->ebx);
            break;

        case SYS_E820_ENTRY:
            regs->eax = (unsigned int)sys_e820_entry_impl(
                            regs->ebx,
                            (sys_e820_entry_t*)regs->ecx);
            break;

        case SYS_NETINFO:
            regs->eax = (unsigned int)sys_netinfo_impl(
                            (sys_netinfo_t*)regs->ebx);
            break;

        case SYS_NET_OP:
            regs->eax = (unsigned int)sys_net_op_impl(
                            (sys_net_op_request_t*)regs->ebx);
            break;

        case SYS_NET_IOCTL:
            regs->eax = (unsigned int)sys_net_ioctl_impl(
                            (int)regs->ebx,
                            regs->ecx,
                            (void*)regs->edx);
            break;

        case SYS_BLOCK_READ_SECTOR:
            regs->eax = (unsigned int)sys_block_read_sector_impl(
                            regs->ebx,
                            (void*)regs->ecx);
            break;

        case SYS_CLOCK_GETTIME:
            regs->eax = (unsigned int)sys_clock_gettime_impl(
                            (int)regs->ebx,
                            (struct user_timespec*)regs->ecx);
            break;

        case SYS_CLOCK_SETTIME:
            regs->eax = (unsigned int)sys_clock_settime_impl(
                            (int)regs->ebx,
                            (const struct user_timespec*)regs->ecx);
            break;

        case SYS_NTP_SYNC:
            regs->eax = (unsigned int)sys_ntp_sync_impl(
                            regs->ebx,
                            (struct user_timespec*)regs->ecx);
            break;

        case SYS_LINK:
            regs->eax = (unsigned int)sys_link_impl((const char*)regs->ebx,
                                                    (const char*)regs->ecx);
            break;

        case SYS_LINKAT:
            regs->eax = (unsigned int)sys_linkat_impl((int)regs->ebx,
                                                      (const char*)regs->ecx,
                                                      (int)regs->edx,
                                                      (const char*)regs->esi,
                                                      regs->edi);
            break;

        case SYS_SYMLINK:
            regs->eax = (unsigned int)sys_symlink_impl((const char*)regs->ebx,
                                                       (const char*)regs->ecx);
            break;

        case SYS_SYMLINKAT:
            regs->eax = (unsigned int)sys_symlinkat_impl((const char*)regs->ebx,
                                                         (int)regs->ecx,
                                                         (const char*)regs->edx);
            break;

        case SYS_READLINK:
            regs->eax = (unsigned int)sys_readlink_impl((const char*)regs->ebx,
                                                        (char*)regs->ecx,
                                                        regs->edx);
            break;

        case SYS_READLINKAT:
            regs->eax = (unsigned int)sys_readlinkat_impl((int)regs->ebx,
                                                          (const char*)regs->ecx,
                                                          (char*)regs->edx,
                                                          regs->esi);
            break;

        case SYS_LSTAT_FULL:
            regs->eax = (unsigned int)sys_lstat_full_impl(
                            (const char*)regs->ebx,
                            (sys_stat_info_t*)regs->ecx);
            break;

        case SYS_FSTATAT_FULL:
            regs->eax = (unsigned int)sys_fstatat_full_impl(
                            (int)regs->ebx,
                            (const char*)regs->ecx,
                            (sys_stat_info_t*)regs->edx,
                            regs->esi);
            break;

        case SYS_CHMOD:
            regs->eax = (unsigned int)sys_chmod_impl((const char*)regs->ebx,
                                                     regs->ecx);
            break;

        case SYS_CHOWN:
            regs->eax = (unsigned int)sys_chown_impl((const char*)regs->ebx,
                                                     regs->ecx,
                                                     regs->edx);
            break;

        case SYS_UTIMENS:
            regs->eax = (unsigned int)sys_utimens_impl(
                            (const char*)regs->ebx,
                            (const struct user_timespec*)regs->ecx);
            break;

        case SYS_UTIMENSAT:
            regs->eax = (unsigned int)sys_utimensat_impl(
                            (int)regs->ebx,
                            (const char*)regs->ecx,
                            (const struct user_timespec*)regs->edx,
                            regs->esi);
            break;

        case SYS_MKNOD:
            regs->eax = (unsigned int)sys_mknod_impl((const char*)regs->ebx,
                                                     regs->ecx,
                                                     regs->edx);
            break;

        case SYS_FTRUNCATE:
            regs->eax = (unsigned int)sys_ftruncate_impl((int)regs->ebx,
                                                         regs->ecx);
            break;

        case SYS_FCHMOD:
            regs->eax = (unsigned int)sys_fchmod_impl((int)regs->ebx,
                                                      regs->ecx);
            break;

        case SYS_FCHOWN:
            regs->eax = (unsigned int)sys_fchown_impl((int)regs->ebx,
                                                      regs->ecx,
                                                      regs->edx);
            break;

        case SYS_FUTIMENS:
            regs->eax = (unsigned int)sys_futimens_impl(
                            (int)regs->ebx,
                            (const struct user_timespec*)regs->ecx);
            break;

        case SYS_GETUID:
            regs->eax = sys_getuid_impl();
            break;

        case SYS_GETEUID:
            regs->eax = sys_geteuid_impl();
            break;

        case SYS_GETGID:
            regs->eax = sys_getgid_impl();
            break;

        case SYS_GETEGID:
            regs->eax = sys_getegid_impl();
            break;

        case SYS_SETUID:
            regs->eax = (unsigned int)sys_setuid_impl(regs->ebx);
            break;

        case SYS_SETGID:
            regs->eax = (unsigned int)sys_setgid_impl(regs->ebx);
            break;

        case SYS_UMASK:
            regs->eax = sys_umask_impl(regs->ebx);
            break;

        case SYS_UNLINKAT:
            regs->eax = (unsigned int)sys_unlinkat_impl((int)regs->ebx,
                                                        (const char*)regs->ecx,
                                                        regs->edx);
            break;

        case SYS_RENAMEAT:
            regs->eax = (unsigned int)sys_renameat_impl((int)regs->ebx,
                                                        (const char*)regs->ecx,
                                                        (int)regs->edx,
                                                        (const char*)regs->esi);
            break;

        default:
            regs->eax = (unsigned int)-ENOSYS;
            break;
    }
}
