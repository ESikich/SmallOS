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
#define SYS_OPEN_MODE_EXCL   0x20u

#define SYS_FD_FLAG_NONBLOCK 0x00000800u
#define SYS_FD_FLAG_CLOEXEC  0x00080000u

#define SYS_PIPE_BUF 4096u

#define SYS_FCNTL_GETFD 1
#define SYS_FCNTL_SETFD 2
#define SYS_FCNTL_GETFL 3
#define SYS_FCNTL_SETFL 4

#define SYS_WAITPID_WNOHANG 1

typedef struct sys_fsinfo {
    unsigned int total_bytes;
    unsigned int used_bytes;
    unsigned int free_bytes;
    /* ABI names are historical; values are ext2 allocation blocks. */
    unsigned int cluster_bytes;
    unsigned int total_clusters;
    unsigned int free_clusters;
} sys_fsinfo_t;

typedef struct sys_fsmap_request {
    /* ABI names are historical; values are ext2 allocation-block indexes. */
    unsigned int start_cluster;
    unsigned int max_clusters;
    unsigned char* states;
    unsigned int out_clusters;
} sys_fsmap_request_t;

typedef struct sys_meminfo {
    unsigned int heap_base;
    unsigned int heap_top;
    unsigned int pmm_free_frames;
    unsigned int pmm_total_frames;
    unsigned int e820_valid;
    unsigned int e820_count;
} sys_meminfo_t;

typedef struct sys_e820_entry {
    unsigned long long base;
    unsigned long long length;
    unsigned int type;
    unsigned int attr;
} sys_e820_entry_t;

typedef struct sys_netinfo {
    unsigned int e1000_link_up;
    unsigned char mac[6];
    unsigned int ipv4_configured;
    unsigned int ip;
    unsigned int netmask;
    unsigned int gateway;
    unsigned int dns;
    unsigned int dhcp_server;
    unsigned int lease_seconds;
    unsigned int max_sockets;
    unsigned int used_sockets;
    unsigned int tcp_sockets;
    unsigned int open_sockets;
    unsigned int bound_sockets;
    unsigned int listening_sockets;
    unsigned int connected_sockets;
    unsigned int tcp_listeners;
    unsigned int tcp_max_listeners;
    unsigned int tcp_connections;
    unsigned int tcp_max_connections;
    unsigned int tcp_established_connections;
    unsigned int tcp_accepted_connections;
    unsigned int tcp_pending_connections;
    unsigned int tcp_syn_recv_connections;
    unsigned int tcp_fin_wait_connections;
    unsigned int tcp_rx_rings;
    unsigned int tcp_tx_rings;
    unsigned int tcp_rx_bytes;
    unsigned int tcp_tx_bytes;
    unsigned int tcp_rx_buffer_bytes;
    unsigned int tcp_tx_buffer_bytes;
    unsigned int tcp_max_rx_buffer_bytes;
    unsigned int tcp_max_tx_buffer_bytes;
} sys_netinfo_t;

#define SYS_USB_DIAG_OP_PORTS 1u
#define SYS_USB_DIAG_OP_DIAG  2u
#define SYS_USB_DIAG_OP_PEEK  3u
#define SYS_USB_DIAG_OP_POWER 4u
#define SYS_USB_DIAG_OP_PORT_SNAPSHOT 5u

#define SYS_USB_PORT_SNAPSHOT_MAX 32u
#define SYS_USB_PORT_ENTRY_CONTROLLER 1u
#define SYS_USB_PORT_ENTRY_PORT       2u

#define SYS_NET_OP_SEND_TEST_FRAME 1u
#define SYS_NET_OP_POLL_ONCE       2u
#define SYS_NET_OP_ARP             3u
#define SYS_NET_OP_PING            4u
#define SYS_NET_OP_DHCP            5u
#define SYS_NET_OP_CONFIGURE       6u
#define SYS_NET_OP_CLEAR_CONFIG    7u

#define SYS_USB_MOUSE_OP_OPEN      1u
#define SYS_USB_MOUSE_OP_POLL      2u
#define SYS_USB_MOUSE_OP_CLOSE     3u

typedef struct sys_usbinfo {
    unsigned int controller_count;
    unsigned int uhci_count;
    unsigned int ohci_count;
    unsigned int ehci_count;
    unsigned int xhci_count;
    unsigned int powered_port_count;
    unsigned int keyboard_active;
    unsigned int keyboard_port;
    unsigned int keyboard_endpoint;
    unsigned int keyboard_packet_size;
    unsigned int keyboard_interval;
    unsigned int keyboard_poll_count;
    unsigned int keyboard_report_count;
    unsigned int keyboard_fail_count;
    unsigned int keyboard_last_cc;
    unsigned int mouse_active;
    unsigned int mouse_port;
    unsigned int mouse_endpoint;
    unsigned int mouse_packet_size;
    unsigned int mouse_interval;
    unsigned int mouse_poll_count;
    unsigned int mouse_report_count;
    unsigned int mouse_fail_count;
    unsigned int mouse_last_cc;
    unsigned int service_active;
    unsigned int storage_active;
    unsigned int storage_port;
    unsigned int last_bar;
    unsigned int last_ports;
    unsigned int last_port_status0;
    unsigned int last_port_status1;
    unsigned char last_bus;
    unsigned char last_slot;
    unsigned char last_func;
    unsigned char last_prog_if;
} sys_usbinfo_t;

typedef struct sys_usb_port_entry {
    unsigned int kind;
    unsigned int controller_index;
    unsigned int bus;
    unsigned int slot;
    unsigned int func;
    unsigned int prog_if;
    unsigned int bar;
    unsigned int port;
    unsigned int port_count;
    unsigned int status;
    unsigned int info;
    unsigned int extra;
} sys_usb_port_entry_t;

typedef struct sys_usb_port_snapshot {
    unsigned int entry_count;
    unsigned int truncated;
    sys_usb_port_entry_t entries[SYS_USB_PORT_SNAPSHOT_MAX];
} sys_usb_port_snapshot_t;

typedef struct sys_mousedebug {
    unsigned int irq_count;
    unsigned int byte_count;
    unsigned int aux_status_count;
    unsigned int packet_count;
    unsigned int vmware_packet_count;
    unsigned int sync_drop_count;
    unsigned int overflow_drop_count;
    unsigned int vmware_enabled;
    unsigned int packet_size;
    unsigned int device_id;
    unsigned int ready;
    unsigned int init_step;
    unsigned int init_fail;
    unsigned int config_before;
    unsigned int config_after;
} sys_mousedebug_t;

typedef struct sys_net_op_request {
    unsigned int op;
    unsigned int target_ip;
    unsigned int sender_ip;
    unsigned int next_hop_ip;
    unsigned int netmask;
    unsigned int gateway;
    unsigned int dns;
    unsigned int dhcp_server;
    unsigned int lease_seconds;
    unsigned char mac[6];
} sys_net_op_request_t;

typedef struct sys_stat_info {
    unsigned int dev;
    unsigned int ino;
    unsigned int mode;
    unsigned int nlink;
    unsigned int uid;
    unsigned int gid;
    unsigned int rdev;
    unsigned int size;
    unsigned int blksize;
    unsigned int blocks;
    unsigned int atime;
    unsigned int mtime;
    unsigned int ctime;
    unsigned int is_dir;
} sys_stat_info_t;

enum {
    SYS_WRITE     = 1,
    SYS_EXIT      = 2,
    SYS_GET_TICKS = 3,
    SYS_PUTC      = 4,
    SYS_READ      = 5,
    SYS_YIELD     = 6,
    SYS_EXEC      = 7,
    SYS_OPEN      = 8,   /* open an ext2 file; returns fd or -1 */
    SYS_CLOSE     = 9,   /* close an fd */
    SYS_FREAD     = 10,  /* read bytes from an open fd into a user buffer */
    SYS_SLEEP     = 11,  /* block for N timer ticks */
    SYS_WRITEFILE = 12,  /* create/overwrite a root-directory file */
    SYS_HALT      = 13,  /* halt the machine */
    SYS_REBOOT    = 14,  /* reboot the machine */
    SYS_WRITEFILE_PATH = 15, /* create/overwrite an ext2 file at any path */
    SYS_BRK       = 16,  /* query or grow the calling process heap break */
    SYS_OPEN_WRITE = 17, /* open an ext2 file for write/truncate */
    SYS_WRITEFD    = 18,  /* write bytes to an open fd */
    SYS_LSEEK      = 19,  /* reposition an open fd */
    SYS_UNLINK     = 20,  /* remove an ext2 file */
    SYS_RENAME     = 21,  /* rename or move an ext2 entry */
    SYS_STAT       = 22,  /* query size / directory status for an ext2 path */

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
    SYS_TERMINAL_SIZE  = 52,  /* write active terminal rows/cols */
    SYS_DISPLAY_INFO   = 53,  /* write framebuffer geometry */
    SYS_DISPLAY_FILL   = 54,  /* fill x,y,w,h with XRGB8888 color */
    SYS_DISPLAY_BLIT   = 55,  /* blit XRGB8888 pixels into framebuffer */
    SYS_DISPLAY_ACQUIRE = 56, /* enter exclusive graphics drawing mode */
    SYS_DISPLAY_RELEASE = 57, /* leave graphics drawing mode */
    SYS_MOUSE_READ      = 58, /* read PS/2 mouse deltas/buttons */
    SYS_INPUT_READ      = 59, /* read queued keyboard/mouse input events */
    SYS_FSINFO          = 60, /* write ext2 volume usage information */
    SYS_FSMAP           = 61, /* write ext2 allocation-block states */
    SYS_GETPID          = 62, /* return current process id */
    SYS_WAITPID         = 63, /* wait for a child pid and copy wait status */
    SYS_KILL            = 64, /* signal/terminate a process by pid */
    SYS_DIRLIST_BATCH   = 65, /* copy a range of directory entries */
    SYS_CLOCK_GETTIME   = 66, /* copy realtime/monotonic timespec */
    SYS_CLOCK_SETTIME   = 67, /* set CLOCK_REALTIME seconds/nanoseconds */
    SYS_NTP_SYNC        = 68, /* query NTP server and set CLOCK_REALTIME */
    SYS_PIPE            = 69, /* create pipe fds */
    SYS_PIPE2           = 70, /* create pipe fds with O_NONBLOCK/O_CLOEXEC */
    SYS_DUP             = 71, /* duplicate fd */
    SYS_DUP2            = 72, /* duplicate fd to exact fd */
    SYS_DUP3            = 73, /* dup2 plus O_CLOEXEC */
    SYS_FORK            = 74, /* clone current process */
    SYS_EXECVE          = 75, /* replace current process image */
    SYS_WAITPID_FG      = 76, /* wait for child as foreground terminal owner */
    SYS_MEMINFO         = 77, /* write kernel memory diagnostic summary */
    SYS_E820_ENTRY      = 78, /* copy one E820 entry; returns total entries */
    SYS_NETINFO         = 79, /* write NIC/IP/socket/TCP diagnostic summary */
    SYS_NET_OP          = 80, /* perform narrow network diagnostic action */
    SYS_BLOCK_READ_SECTOR = 81, /* copy one mounted block-device sector */
    SYS_ATA_READ_SECTOR = SYS_BLOCK_READ_SECTOR, /* legacy alias */
    SYS_EXEC_FG         = 82, /* spawn an ELF in its own foreground job group */
    SYS_PTY_OPEN        = 83, /* create master/slave pseudo-terminal fds */
    SYS_PTY_SET_SIZE    = 84, /* set pseudo-terminal rows/cols */
    SYS_STAT_FULL       = 85, /* copy full POSIX-shaped stat info for path */
    SYS_FSTAT_FULL      = 86, /* copy full POSIX-shaped stat info for fd */
    SYS_USB_MOUSE_OP    = 87, /* temporary USB mouse diagnostic session */
    SYS_USBINFO         = 88, /* write USB controller/HID diagnostic summary */
    SYS_MOUSE_DEBUG     = 89, /* write PS/2 mouse diagnostic counters */
    SYS_USB_DIAG_OP     = 90  /* perform narrow USB diagnostic action */
};

#endif
