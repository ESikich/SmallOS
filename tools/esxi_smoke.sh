#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/esxi_smoke.sh --host HOST [options]

Deploy SmallOS to ESXi, reboot the VM, and verify the serial boot transcript.

Options:
  --host HOST             ESXi host or IP address (or ESXI_HOST)
  --user USER             ESXi SSH user (default: ESXI_USER or root)
  --password-env NAME     Environment variable containing the SSH password
                          (default: ESXI_PASSWORD)
  --datastore NAME        ESXi datastore name (default: ESXI_DATASTORE or datastore1)
  --vm-dir NAME           Directory under the datastore (default: ESXI_VM_DIR or SmallOS)
  --vm-name NAME          VM inventory name (default: ESXI_VM_NAME or --vm-dir)
  --display-backend NAME  SmallOS display backend: auto or vga (default: auto)
  --serial-file PATH      Serial log path; defaults to serial0.fileName from the VMX
  --timeout SEC           Seconds to wait for SmallOS ready (default: 180)
  --skip-deploy           Only inspect the current serial log
  --no-clear              Do not clear the serial log before deploying
  --no-force              Do not pass --force to deploy_esxi.sh
  -h, --help              Show this help

Examples:
  ESXI_HOST=10.10.0.13 tools/esxi_smoke.sh
  ESXI_PASSWORD='...' tools/esxi_smoke.sh --host 10.10.0.13
  tools/esxi_smoke.sh --host 10.10.0.13 --display-backend vga
EOF
}

die() {
    printf 'esxi_smoke: %s\n' "$*" >&2
    exit 1
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ESXI_HOST="${ESXI_HOST:-}"
ESXI_USER="${ESXI_USER:-root}"
ESXI_PASSWORD_ENV="${ESXI_PASSWORD_ENV:-ESXI_PASSWORD}"
ESXI_DATASTORE="${ESXI_DATASTORE:-datastore1}"
ESXI_VM_DIR="${ESXI_VM_DIR:-SmallOS}"
ESXI_VM_NAME="${ESXI_VM_NAME:-}"
DISPLAY_BACKEND="${DISPLAY_BACKEND:-auto}"
ESXI_SERIAL_FILE="${ESXI_SERIAL_FILE:-}"
TIMEOUT="${ESXI_SMOKE_TIMEOUT:-180}"
SKIP_DEPLOY=0
CLEAR_LOG=1
FORCE=1

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
        --vm-name)
            [ "$#" -ge 2 ] || die "--vm-name requires a value"
            ESXI_VM_NAME="$2"
            shift 2
            ;;
        --display-backend)
            [ "$#" -ge 2 ] || die "--display-backend requires a value"
            DISPLAY_BACKEND="$2"
            shift 2
            ;;
        --serial-file)
            [ "$#" -ge 2 ] || die "--serial-file requires a value"
            ESXI_SERIAL_FILE="$2"
            shift 2
            ;;
        --timeout)
            [ "$#" -ge 2 ] || die "--timeout requires a value"
            TIMEOUT="$2"
            shift 2
            ;;
        --skip-deploy)
            SKIP_DEPLOY=1
            shift
            ;;
        --no-clear)
            CLEAR_LOG=0
            shift
            ;;
        --no-force)
            FORCE=0
            shift
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

case "$DISPLAY_BACKEND" in
    auto|vga) ;;
    *) die "--display-backend must be auto or vga" ;;
esac

case "$TIMEOUT" in
    ''|*[!0-9]*) die "--timeout must be a non-negative integer" ;;
esac

common_args=(
    --host "$ESXI_HOST"
    --user "$ESXI_USER"
    --password-env "$ESXI_PASSWORD_ENV"
    --datastore "$ESXI_DATASTORE"
    --vm-dir "$ESXI_VM_DIR"
)

if [ -n "$ESXI_SERIAL_FILE" ]; then
    common_args+=(--serial-file "$ESXI_SERIAL_FILE")
fi

serial_tool="$repo_root/tools/esxi_serial_log.sh"
deploy_tool="$repo_root/tools/deploy_esxi.sh"

if [ "$CLEAR_LOG" -eq 1 ]; then
    "$serial_tool" "${common_args[@]}" --clear
fi

if [ "$SKIP_DEPLOY" -eq 0 ]; then
    deploy_args=(
        --host "$ESXI_HOST"
        --user "$ESXI_USER"
        --password-env "$ESXI_PASSWORD_ENV"
        --datastore "$ESXI_DATASTORE"
        --vm-dir "$ESXI_VM_DIR"
        --display-backend "$DISPLAY_BACKEND"
        --attach-and-reboot
    )
    if [ -n "$ESXI_VM_NAME" ]; then
        deploy_args+=(--vm-name "$ESXI_VM_NAME")
    fi
    if [ "$FORCE" -eq 1 ]; then
        deploy_args+=(--force)
    fi
    "$deploy_tool" "${deploy_args[@]}"
fi

"$serial_tool" "${common_args[@]}" --wait-for "SmallOS ready" --timeout "$TIMEOUT" --lines 120
log="$("$serial_tool" "${common_args[@]}" --tail --lines 1000)"

markers=(
    "boot: PASS terminal:"
    "boot: PASS boot info: SMOS v3 contract"
    "boot: PASS mouse: PS/2 packet stream enabled"
    "boot: PASS ata: primary channel ready"
    "boot: PASS e1000: Intel PRO/1000 ready"
    "dhcp: bound"
    "boot: PASS dhcp: IPv4 lease acquired"
    "boot: PASS ext2: volume mounted"
    "SmallOS ready"
)

missing=0
for marker in "${markers[@]}"; do
    if printf '%s\n' "$log" | grep -F -- "$marker" >/dev/null 2>&1; then
        printf '[PASS] %s\n' "$marker"
    else
        printf '[FAIL] missing marker: %s\n' "$marker" >&2
        missing=1
    fi
done

if [ "$missing" -ne 0 ]; then
    printf '\nLast serial lines:\n' >&2
    printf '%s\n' "$log" | tail -n 120 >&2
    exit 1
fi

printf 'ESXi smoke: PASS\n'
