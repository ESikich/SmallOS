#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/esxi_serial_log.sh --host HOST [options]

Read, clear, follow, or wait on the SmallOS VM serial log stored on ESXi.

Options:
  --host HOST          ESXi host or IP address (or ESXI_HOST)
  --user USER          ESXi SSH user (default: ESXI_USER or root)
  --password-env NAME  Environment variable containing the SSH password
                       (default: ESXI_PASSWORD)
  --datastore NAME     ESXi datastore name (default: ESXI_DATASTORE or datastore1)
  --vm-dir NAME        Directory under the datastore (default: ESXI_VM_DIR or SmallOS)
  --serial-file PATH   Serial log path; defaults to serial0.fileName from the VMX
  --lines N            Lines to show for tail/follow (default: 200)
  --tail               Print the last N lines (default action)
  --follow             Follow the log with tail -F
  --clear              Truncate the serial log
  --print-path         Print the resolved serial log path
  --wait-for TEXT      Wait until TEXT appears in the log
  --timeout SEC        Wait timeout for --wait-for (default: 120)
  -h, --help           Show this help
EOF
}

die() {
    printf 'esxi_serial_log: %s\n' "$*" >&2
    exit 1
}

require_tool() {
    command -v "$1" >/dev/null 2>&1 || die "missing required tool: $1"
}

remote_quote() {
    case "$1" in
        *$'\n'*|*$'\r'*)
            die "remote arguments must not contain newlines"
            ;;
    esac
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

ESXI_HOST="${ESXI_HOST:-}"
ESXI_USER="${ESXI_USER:-root}"
ESXI_PASSWORD_ENV="${ESXI_PASSWORD_ENV:-ESXI_PASSWORD}"
ESXI_DATASTORE="${ESXI_DATASTORE:-datastore1}"
ESXI_VM_DIR="${ESXI_VM_DIR:-SmallOS}"
ESXI_SERIAL_FILE="${ESXI_SERIAL_FILE:-}"
LINES=200
ACTION=tail
WAIT_FOR=""
TIMEOUT=120

while [ "$#" -gt 0 ]; do
    case "$1" in
        --host)
            [ "$#" -ge 2 ] || die "--host requires a value"
            ESXI_HOST="$2"
            shift 2
            ;;
        --user)
            [ "$#" -ge 2 ] || die "--user requires a value"
            ESXI_USER="$2"
            shift 2
            ;;
        --password-env)
            [ "$#" -ge 2 ] || die "--password-env requires a value"
            ESXI_PASSWORD_ENV="$2"
            shift 2
            ;;
        --datastore)
            [ "$#" -ge 2 ] || die "--datastore requires a value"
            ESXI_DATASTORE="$2"
            shift 2
            ;;
        --vm-dir)
            [ "$#" -ge 2 ] || die "--vm-dir requires a value"
            ESXI_VM_DIR="$2"
            shift 2
            ;;
        --serial-file)
            [ "$#" -ge 2 ] || die "--serial-file requires a value"
            ESXI_SERIAL_FILE="$2"
            shift 2
            ;;
        --lines)
            [ "$#" -ge 2 ] || die "--lines requires a value"
            LINES="$2"
            shift 2
            ;;
        --tail)
            ACTION=tail
            shift
            ;;
        --follow)
            ACTION=follow
            shift
            ;;
        --clear)
            ACTION=clear
            shift
            ;;
        --print-path)
            ACTION=path
            shift
            ;;
        --wait-for)
            [ "$#" -ge 2 ] || die "--wait-for requires a value"
            ACTION=wait
            WAIT_FOR="$2"
            shift 2
            ;;
        --timeout)
            [ "$#" -ge 2 ] || die "--timeout requires a value"
            TIMEOUT="$2"
            shift 2
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

[ -n "$ESXI_HOST" ] || die "missing ESXi host; pass --host or set ESXI_HOST"

case "$LINES" in
    ''|*[!0-9]*) die "--lines must be a non-negative integer" ;;
esac

case "$TIMEOUT" in
    ''|*[!0-9]*) die "--timeout must be a non-negative integer" ;;
esac

require_tool ssh

ESXI_PASSWORD_VALUE="${!ESXI_PASSWORD_ENV:-}"
USE_SSHPASS=0
if [ -n "$ESXI_PASSWORD_VALUE" ]; then
    require_tool sshpass
    USE_SSHPASS=1
fi

remote_user_host="${ESXI_USER}@${ESXI_HOST}"
SSH_OPTS=(-o StrictHostKeyChecking=accept-new)
SERIAL_FILE_ARG="${ESXI_SERIAL_FILE:-__SMALLOS_DEFAULT_SERIAL__}"
WAIT_FOR_ARG="${WAIT_FOR:-__SMALLOS_NO_WAIT_MARKER__}"
REMOTE_ARGS=(
    "$ESXI_DATASTORE"
    "$ESXI_VM_DIR"
    "$SERIAL_FILE_ARG"
    "$ACTION"
    "$LINES"
    "$WAIT_FOR_ARG"
    "$TIMEOUT"
)
REMOTE_CMD="sh -s --"
for arg in "${REMOTE_ARGS[@]}"; do
    REMOTE_CMD="${REMOTE_CMD} $(remote_quote "$arg")"
done

if [ "$USE_SSHPASS" -eq 1 ]; then
    export SSHPASS="$ESXI_PASSWORD_VALUE"
    sshpass -e ssh "${SSH_OPTS[@]}" "$remote_user_host" "$REMOTE_CMD" <<'REMOTE'
set -eu

datastore="$1"
vm_dir="$2"
serial_file="$3"
[ "$serial_file" = "__SMALLOS_DEFAULT_SERIAL__" ] && serial_file=""
action="$4"
lines="$5"
wait_for="$6"
[ "$wait_for" = "__SMALLOS_NO_WAIT_MARKER__" ] && wait_for=""
timeout="$7"
remote_dir="/vmfs/volumes/${datastore}/${vm_dir}"

fail() {
    printf 'esxi_serial_log: %s\n' "$*" >&2
    exit 1
}

resolve_bracket_path() {
    input="$1"
    case "$input" in
        \[*\]*)
            ds="${input#\[}"
            ds="${ds%%\]*}"
            rest="${input#*\]}"
            rest="${rest# }"
            [ -n "$ds" ] || fail "bad datastore path: $input"
            [ -n "$rest" ] || fail "bad datastore path: $input"
            printf '/vmfs/volumes/%s/%s\n' "$ds" "$rest"
            ;;
        /*)
            printf '%s\n' "$input"
            ;;
        *)
            printf '%s/%s\n' "$remote_dir" "$input"
            ;;
    esac
}

resolve_serial_path() {
    if [ -n "$serial_file" ]; then
        resolve_bracket_path "$serial_file"
        return
    fi

    [ -d "$remote_dir" ] || fail "VM directory not found: $remote_dir"
    vmx_count="$(find "$remote_dir" -maxdepth 1 -name '*.vmx' | sed '/^$/d' | awk 'END { print NR }')"
    [ "$vmx_count" = "1" ] || fail "expected one .vmx in $remote_dir, found $vmx_count"
    vmx="$(find "$remote_dir" -maxdepth 1 -name '*.vmx' | sed -n '1p')"
    name="$(awk -F= '
        $1 ~ /^[[:space:]]*serial0\.fileName[[:space:]]*$/ {
            value = $2
            sub(/^[[:space:]]*/, "", value)
            sub(/[[:space:]]*$/, "", value)
            sub(/^"/, "", value)
            sub(/"$/, "", value)
            print value
            exit
        }
    ' "$vmx")"
    [ -n "$name" ] || fail "serial0.fileName not found in $vmx; pass --serial-file"
    resolve_bracket_path "$name"
}

path="$(resolve_serial_path)"

case "$action" in
    path)
        printf '%s\n' "$path"
        ;;
    clear)
        mkdir -p "$(dirname "$path")"
        : > "$path"
        printf 'cleared %s\n' "$path"
        ;;
    tail)
        [ -f "$path" ] || fail "serial log not found: $path"
        tail -n "$lines" "$path"
        ;;
    follow)
        mkdir -p "$(dirname "$path")"
        [ -f "$path" ] || : > "$path"
        tail -n "$lines" -F "$path"
        ;;
    wait)
        [ -n "$wait_for" ] || fail "--wait-for requires a non-empty marker"
        elapsed=0
        while [ "$elapsed" -le "$timeout" ]; do
            if [ -f "$path" ] && grep -F -- "$wait_for" "$path" >/dev/null 2>&1; then
                printf 'found marker after %ss: %s\n' "$elapsed" "$wait_for"
                exit 0
            fi
            sleep 1
            elapsed=$((elapsed + 1))
        done
        printf 'timed out waiting for marker: %s\n' "$wait_for" >&2
        if [ -f "$path" ]; then
            tail -n "$lines" "$path" >&2 || true
        fi
        exit 1
        ;;
    *)
        fail "unknown action: $action"
        ;;
esac
REMOTE
else
    ssh "${SSH_OPTS[@]}" "$remote_user_host" "$REMOTE_CMD" <<'REMOTE'
set -eu

datastore="$1"
vm_dir="$2"
serial_file="$3"
[ "$serial_file" = "__SMALLOS_DEFAULT_SERIAL__" ] && serial_file=""
action="$4"
lines="$5"
wait_for="$6"
[ "$wait_for" = "__SMALLOS_NO_WAIT_MARKER__" ] && wait_for=""
timeout="$7"
remote_dir="/vmfs/volumes/${datastore}/${vm_dir}"

fail() {
    printf 'esxi_serial_log: %s\n' "$*" >&2
    exit 1
}

resolve_bracket_path() {
    input="$1"
    case "$input" in
        \[*\]*)
            ds="${input#\[}"
            ds="${ds%%\]*}"
            rest="${input#*\]}"
            rest="${rest# }"
            [ -n "$ds" ] || fail "bad datastore path: $input"
            [ -n "$rest" ] || fail "bad datastore path: $input"
            printf '/vmfs/volumes/%s/%s\n' "$ds" "$rest"
            ;;
        /*)
            printf '%s\n' "$input"
            ;;
        *)
            printf '%s/%s\n' "$remote_dir" "$input"
            ;;
    esac
}

resolve_serial_path() {
    if [ -n "$serial_file" ]; then
        resolve_bracket_path "$serial_file"
        return
    fi

    [ -d "$remote_dir" ] || fail "VM directory not found: $remote_dir"
    vmx_count="$(find "$remote_dir" -maxdepth 1 -name '*.vmx' | sed '/^$/d' | awk 'END { print NR }')"
    [ "$vmx_count" = "1" ] || fail "expected one .vmx in $remote_dir, found $vmx_count"
    vmx="$(find "$remote_dir" -maxdepth 1 -name '*.vmx' | sed -n '1p')"
    name="$(awk -F= '
        $1 ~ /^[[:space:]]*serial0\.fileName[[:space:]]*$/ {
            value = $2
            sub(/^[[:space:]]*/, "", value)
            sub(/[[:space:]]*$/, "", value)
            sub(/^"/, "", value)
            sub(/"$/, "", value)
            print value
            exit
        }
    ' "$vmx")"
    [ -n "$name" ] || fail "serial0.fileName not found in $vmx; pass --serial-file"
    resolve_bracket_path "$name"
}

path="$(resolve_serial_path)"

case "$action" in
    path)
        printf '%s\n' "$path"
        ;;
    clear)
        mkdir -p "$(dirname "$path")"
        : > "$path"
        printf 'cleared %s\n' "$path"
        ;;
    tail)
        [ -f "$path" ] || fail "serial log not found: $path"
        tail -n "$lines" "$path"
        ;;
    follow)
        mkdir -p "$(dirname "$path")"
        [ -f "$path" ] || : > "$path"
        tail -n "$lines" -F "$path"
        ;;
    wait)
        [ -n "$wait_for" ] || fail "--wait-for requires a non-empty marker"
        elapsed=0
        while [ "$elapsed" -le "$timeout" ]; do
            if [ -f "$path" ] && grep -F -- "$wait_for" "$path" >/dev/null 2>&1; then
                printf 'found marker after %ss: %s\n' "$elapsed" "$wait_for"
                exit 0
            fi
            sleep 1
            elapsed=$((elapsed + 1))
        done
        printf 'timed out waiting for marker: %s\n' "$wait_for" >&2
        if [ -f "$path" ]; then
            tail -n "$lines" "$path" >&2 || true
        fi
        exit 1
        ;;
    *)
        fail "unknown action: $action"
        ;;
esac
REMOTE
fi
