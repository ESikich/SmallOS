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
setconf CONFIG_LN y
setconf CONFIG_LS y
setconf CONFIG_MKDIR y
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
unsetconf CONFIG_ASH_JOB_CONTROL
setconf CONFIG_ASH_ALIAS y
setconf CONFIG_ASH_EXPAND_PRMT y
setconf CONFIG_ASH_ECHO y
setconf CONFIG_ASH_PRINTF y
setconf CONFIG_ASH_TEST y
setconf CONFIG_FEATURE_SH_STANDALONE y
setconf CONFIG_FEATURE_SH_NOFORK y
setconf CONFIG_FEATURE_SH_READ_FRAC y
unsetconf CONFIG_HUSH

unsetconf CONFIG_MOUNT
unsetconf CONFIG_UMOUNT
unsetconf CONFIG_INIT
unsetconf CONFIG_LOGIN
unsetconf CONFIG_GETTY
unsetconf CONFIG_HTTPD
unsetconf CONFIG_NC
unsetconf CONFIG_NETCAT
unsetconf CONFIG_WGET
unsetconf CONFIG_IFCONFIG
unsetconf CONFIG_ROUTE
unsetconf CONFIG_IP
unsetconf CONFIG_PING
unsetconf CONFIG_PING6

set +o pipefail
yes "" | make -C "$src" O="$out" ARCH=i386 CC="$cc" oldconfig >/dev/null
oldconfig_status=${PIPESTATUS[1]}
set -o pipefail
exit "$oldconfig_status"
