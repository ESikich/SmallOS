#!/usr/bin/env bash
set -euo pipefail

repo=${1:?repo root}
src=${2:?busybox source}
out=${3:?busybox output}
cc=${4:?compiler}
libgcc=${5:?libgcc file}
obj_dir=${6:?object dir}

make -C "$src" O="$out" ARCH=i386 CC="$cc" allnoconfig >/dev/null

cfg="$out/.config"

setconf() {
    local key=$1
    local value=$2
    local tmp
    tmp=$(mktemp)
    awk -v key="$key" -v line="${key}=${value}" '
        $0 == "# " key " is not set" || index($0, key "=") == 1 {
            if (!done) {
                print line
                done = 1
            }
            next
        }
        { print }
        END {
            if (!done) print line
        }
    ' "$cfg" > "$tmp"
    mv "$tmp" "$cfg"
}

unsetconf() {
    local key=$1
    local tmp
    tmp=$(mktemp)
    awk -v key="$key" -v line="# ${key} is not set" '
        $0 == "# " key " is not set" || index($0, key "=") == 1 {
            if (!done) {
                print line
                done = 1
            }
            next
        }
        { print }
        END {
            if (!done) print line
        }
    ' "$cfg" > "$tmp"
    mv "$tmp" "$cfg"
}

gcc_include=$("$cc" -m32 -print-file-name=include)
libgcc_dir=$(dirname "$libgcc")
extra_cflags="-m32 -ffreestanding -fno-pie -fno-stack-protector -fno-asynchronous-unwind-tables -fno-unwind-tables -nostdinc -isystem $gcc_include -I$repo/src/user/include -I$repo/src/kernel -DSMALLOS"
extra_ldflags="-m32 -no-pie -nostdlib -nostartfiles -Wl,--allow-multiple-definition -Wl,-Ttext-segment,0x400000 -Wl,-e,_start -L$repo/$obj_dir/user/lib -L$libgcc_dir -Wl,--whole-archive -lposix -lc -lm -Wl,--no-whole-archive -lgcc"
extra_ldlibs=""

setconf CONFIG_BUSYBOX y
setconf CONFIG_STATIC y
unsetconf CONFIG_PIE
unsetconf CONFIG_FEATURE_INSTALLER
unsetconf CONFIG_INSTALL_APPLET_SYMLINKS
unsetconf CONFIG_INSTALL_APPLET_HARDLINKS
unsetconf CONFIG_INSTALL_APPLET_SCRIPT_WRAPPERS
setconf CONFIG_INSTALL_APPLET_DONT y
setconf CONFIG_BUSYBOX_EXEC_PATH "\"/usr/bin/busybox\""
setconf CONFIG_CROSS_COMPILER_PREFIX "\"\""
setconf CONFIG_SYSROOT "\"\""
setconf CONFIG_EXTRA_CFLAGS "\"$extra_cflags\""
setconf CONFIG_EXTRA_LDFLAGS "\"$extra_ldflags\""
setconf CONFIG_EXTRA_LDLIBS "\"$extra_ldlibs\""
unsetconf CONFIG_STATIC_LIBGCC

setconf CONFIG_CAT y
setconf CONFIG_BASENAME y
setconf CONFIG_CHMOD y
setconf CONFIG_CHGRP y
setconf CONFIG_CHOWN y
setconf CONFIG_CMP y
setconf CONFIG_CP y
setconf CONFIG_CUT y
setconf CONFIG_DIFF y
setconf CONFIG_DIRNAME y
setconf CONFIG_ECHO y
setconf CONFIG_ENV y
setconf CONFIG_FALSE y
setconf CONFIG_FIND y
setconf CONFIG_HEAD y
setconf CONFIG_ID y
setconf CONFIG_KILL y
setconf CONFIG_LINK y
setconf CONFIG_LN y
setconf CONFIG_LS y
setconf CONFIG_MKDIR y
setconf CONFIG_MKFIFO y
setconf CONFIG_MKNOD y
setconf CONFIG_MV y
setconf CONFIG_MD5SUM y
setconf CONFIG_PRINTENV y
setconf CONFIG_PRINTF y
setconf CONFIG_PWD y
setconf CONFIG_REALPATH y
setconf CONFIG_RM y
setconf CONFIG_RMDIR y
setconf CONFIG_SEQ y
setconf CONFIG_SHA1SUM y
setconf CONFIG_SHA256SUM y
setconf CONFIG_SLEEP y
setconf CONFIG_SORT y
setconf CONFIG_STAT y
setconf CONFIG_SYNC y
setconf CONFIG_TEE y
setconf CONFIG_TOUCH y
setconf CONFIG_TR y
setconf CONFIG_TRUE y
setconf CONFIG_TTY y
setconf CONFIG_UNAME y
setconf CONFIG_UNIQ y
setconf CONFIG_UNLINK y
setconf CONFIG_USLEEP y
setconf CONFIG_WC y
setconf CONFIG_WHOAMI y
setconf CONFIG_XARGS y
setconf CONFIG_YES y
setconf CONFIG_TEST y
setconf CONFIG_EXPR y
setconf CONFIG_GREP y
setconf CONFIG_SED y
setconf CONFIG_AWK y
setconf CONFIG_WHICH y
setconf CONFIG_READLINK y
setconf CONFIG_DF y
setconf CONFIG_DU y
setconf CONFIG_FREE y
setconf CONFIG_PS y
setconf CONFIG_TOP y
setconf CONFIG_DATE y
setconf CONFIG_DD y
setconf CONFIG_GUNZIP y
setconf CONFIG_GZIP y
setconf CONFIG_OD y
setconf CONFIG_HEXDUMP y
setconf CONFIG_TAR y
setconf CONFIG_FEATURE_SEAMLESS_GZ y
setconf CONFIG_FEATURE_TAR_AUTODETECT y
setconf CONFIG_FEATURE_TAR_CREATE y
setconf CONFIG_FEATURE_TAR_FROM y
setconf CONFIG_FEATURE_TAR_LONG_OPTIONS y
setconf CONFIG_FEATURE_GREP_CONTEXT y
setconf CONFIG_FEATURE_DD_IBS_OBS y
setconf CONFIG_FEATURE_DD_STATUS y
setconf CONFIG_FEATURE_HUMAN_READABLE y
setconf CONFIG_FEATURE_LS_FILETYPES y
setconf CONFIG_FEATURE_LS_SORTFILES y
setconf CONFIG_FEATURE_PS_WIDE y
setconf CONFIG_FEATURE_STAT_FORMAT y
setconf CONFIG_FEATURE_STAT_FILESYSTEM y
setconf CONFIG_FEATURE_TOP_CPU_USAGE_PERCENTAGE y
setconf CONFIG_FEATURE_FIND_TYPE y
setconf CONFIG_FEATURE_FIND_PRINT0 y
setconf CONFIG_FEATURE_XARGS_SUPPORT_ZERO_TERM y

setconf CONFIG_SH_IS_ASH y
unsetconf CONFIG_SH_IS_HUSH
unsetconf CONFIG_SH_IS_NONE
setconf CONFIG_SHELL_ASH y
unsetconf CONFIG_SHELL_HUSH
setconf CONFIG_ASH y
setconf CONFIG_ASH_INTERNAL_GLOB y
setconf CONFIG_ASH_JOB_CONTROL y
setconf CONFIG_ASH_ALIAS y
setconf CONFIG_ASH_EXPAND_PRMT y
setconf CONFIG_ASH_ECHO y
setconf CONFIG_ASH_PRINTF y
setconf CONFIG_ASH_TEST y
setconf CONFIG_FEATURE_SH_STANDALONE y
setconf CONFIG_FEATURE_SH_NOFORK y
setconf CONFIG_FEATURE_SH_READ_FRAC y
unsetconf CONFIG_HUSH

setconf CONFIG_MOUNT y
setconf CONFIG_UMOUNT y
setconf CONFIG_INIT y
unsetconf CONFIG_FEATURE_USE_INITTAB
unsetconf CONFIG_FEATURE_INIT_SCTTY
unsetconf CONFIG_FEATURE_INIT_SYSLOG
unsetconf CONFIG_FEATURE_INIT_QUIET
unsetconf CONFIG_FEATURE_INIT_COREDUMPS
setconf CONFIG_INIT_TERMINAL_TYPE "\"\""
unsetconf CONFIG_FEATURE_INIT_MODIFY_CMDLINE
unsetconf CONFIG_FEATURE_SHADOWPASSWDS
setconf CONFIG_LOGIN y
unsetconf CONFIG_LOGIN_SESSION_AS_CHILD
unsetconf CONFIG_LOGIN_SCRIPTS
unsetconf CONFIG_FEATURE_NOLOGIN
unsetconf CONFIG_FEATURE_SECURETTY
setconf CONFIG_GETTY y
unsetconf CONFIG_FEATURE_PASSWD_WEAK_CHECK
setconf CONFIG_HTTPD y
unsetconf CONFIG_FEATURE_HTTPD_RANGES
unsetconf CONFIG_FEATURE_HTTPD_SETUID
unsetconf CONFIG_FEATURE_HTTPD_BASIC_AUTH
unsetconf CONFIG_FEATURE_HTTPD_AUTH_MD5
unsetconf CONFIG_FEATURE_HTTPD_CGI
unsetconf CONFIG_FEATURE_HTTPD_CONFIG_WITH_SCRIPT_INTERPR
unsetconf CONFIG_FEATURE_HTTPD_SET_REMOTE_PORT_TO_ENV
unsetconf CONFIG_FEATURE_HTTPD_ENCODE_URL_STR
unsetconf CONFIG_FEATURE_HTTPD_ERROR_PAGES
unsetconf CONFIG_FEATURE_HTTPD_PROXY
unsetconf CONFIG_FEATURE_HTTPD_GZIP
unsetconf CONFIG_FEATURE_HTTPD_ETAG
unsetconf CONFIG_FEATURE_HTTPD_LAST_MODIFIED
unsetconf CONFIG_FEATURE_HTTPD_DATE
unsetconf CONFIG_FEATURE_HTTPD_ACL_IP
setconf CONFIG_HOSTNAME y
setconf CONFIG_DNSDOMAINNAME y
setconf CONFIG_NC y
unsetconf CONFIG_NETCAT
setconf CONFIG_FTPGET y
setconf CONFIG_FTPPUT y
unsetconf CONFIG_FEATURE_FTPGETPUT_LONG_OPTIONS
setconf CONFIG_WGET y
unsetconf CONFIG_FEATURE_WGET_LONG_OPTIONS
unsetconf CONFIG_FEATURE_WGET_STATUSBAR
unsetconf CONFIG_FEATURE_WGET_FTP
unsetconf CONFIG_FEATURE_WGET_AUTHENTICATION
unsetconf CONFIG_FEATURE_WGET_TIMEOUT
unsetconf CONFIG_FEATURE_WGET_HTTPS
unsetconf CONFIG_FEATURE_WGET_OPENSSL
setconf CONFIG_IFCONFIG y
setconf CONFIG_FEATURE_IFCONFIG_STATUS y
setconf CONFIG_ROUTE y
setconf CONFIG_ARP y
setconf CONFIG_IP y
unsetconf CONFIG_IPADDR
unsetconf CONFIG_IPLINK
unsetconf CONFIG_IPROUTE
unsetconf CONFIG_IPTUNNEL
unsetconf CONFIG_IPRULE
setconf CONFIG_IPNEIGH y
setconf CONFIG_FEATURE_IP_ADDRESS y
setconf CONFIG_FEATURE_IP_LINK y
setconf CONFIG_FEATURE_IP_ROUTE y
setconf CONFIG_FEATURE_IP_ROUTE_DIR "\"/etc/iproute2\""
unsetconf CONFIG_FEATURE_IP_TUNNEL
unsetconf CONFIG_FEATURE_IP_RULE
setconf CONFIG_FEATURE_IP_NEIGH y
unsetconf CONFIG_FEATURE_IP_RARE_PROTOCOLS
setconf CONFIG_IPCALC y
unsetconf CONFIG_FEATURE_IPCALC_LONG_OPTIONS
unsetconf CONFIG_FEATURE_IPCALC_FANCY
setconf CONFIG_NETSTAT y
unsetconf CONFIG_FEATURE_NETSTAT_WIDE
unsetconf CONFIG_FEATURE_NETSTAT_PRG
setconf CONFIG_NSLOOKUP y
unsetconf CONFIG_FEATURE_NSLOOKUP_BIG
unsetconf CONFIG_FEATURE_NSLOOKUP_LONG_OPTIONS
setconf CONFIG_WHOIS y
setconf CONFIG_PING y
unsetconf CONFIG_FEATURE_FANCY_PING
unsetconf CONFIG_PING6
setconf CONFIG_PSCAN y
setconf CONFIG_TCPSVD y
setconf CONFIG_UDPSVD y
setconf CONFIG_TFTP y
setconf CONFIG_TFTPD y
setconf CONFIG_FEATURE_TFTP_GET y
setconf CONFIG_FEATURE_TFTP_PUT y
unsetconf CONFIG_FEATURE_TFTP_PROGRESS_BAR
unsetconf CONFIG_FEATURE_TFTP_HPA_COMPAT
unsetconf CONFIG_FEATURE_TFTP_BLOCKSIZE
unsetconf CONFIG_TFTP_DEBUG
setconf CONFIG_UDHCPC y
unsetconf CONFIG_FEATURE_UDHCPC_ARPING
setconf CONFIG_FEATURE_UDHCPC_SANITIZEOPT y
setconf CONFIG_UDHCPC_DEFAULT_SCRIPT "\"/usr/share/udhcpc/default.script\""
setconf CONFIG_UDHCPC_DEFAULT_INTERFACE "\"eth0\""
unsetconf CONFIG_FEATURE_UDHCP_PORT
setconf CONFIG_UDHCP_DEBUG 1
setconf CONFIG_UDHCPC_SLACK_FOR_BUGGY_SERVERS 80
unsetconf CONFIG_FEATURE_UDHCP_RFC3397
unsetconf CONFIG_FEATURE_UDHCP_8021Q
setconf CONFIG_UDHCPD y
unsetconf CONFIG_FEATURE_UDHCPD_BASE_IP_ON_MAC
unsetconf CONFIG_FEATURE_UDHCPD_WRITE_LEASES_EARLY
setconf CONFIG_DHCPD_LEASES_FILE "\"/var/lib/misc/udhcpd.leases\""
setconf CONFIG_DUMPLEASES y
unsetconf CONFIG_DHCPRELAY

set +o pipefail
yes "" | make -C "$src" O="$out" ARCH=i386 CC="$cc" oldconfig >/dev/null
oldconfig_status=${PIPESTATUS[1]}
set -o pipefail
exit "$oldconfig_status"
