#!/usr/bin/env bash
set -euo pipefail

usage() {
    cat <<'EOF'
Usage: tools/deploy_esxi.sh --host HOST [options]

Build, upload, and import the SmallOS ESXi VMDK with vmkfstools.

Options:
  --host HOST             ESXi host or IP address (or ESXI_HOST)
  --user USER             ESXi SSH user (default: ESXI_USER or root)
  --password-env NAME     Environment variable containing the SSH password
                          (default: ESXI_PASSWORD)
  --datastore NAME        ESXi datastore name (default: ESXI_DATASTORE or datastore1)
  --vm-dir NAME           Directory under the datastore (default: ESXI_VM_DIR or SmallOS)
  --display-backend NAME  SmallOS display backend: auto or vga (default: auto)
  --size SIZE             Optional raw disk size/padding passed to make esxi-vmdk
                          (default: use the assembled image size)
  --local-vmdk PATH       Upload an existing local VMDK instead of the default build output
  --remote-name NAME      Imported VMFS VMDK name (default: smallos-esxi-vmfs.vmdk)
  --upload-name NAME      Temporary uploaded VMDK name (default: smallos-esxi-upload.vmdk)
  --vm-name NAME          VM inventory name for disk replacement/reboot
                          (default: ESXI_VM_NAME or --vm-dir)
  --replace-vm-disk      Replace the VM disk at the selected controller/unit
  --attach-and-reboot    Alias for --replace-vm-disk --power-on
  --disk-controller N    VM disk controller number (default: 0)
  --disk-unit N          VM disk unit number (default: 0)
  --controller-type TYPE VM disk controller type (default: ide)
  --power-on             Power on the VM after replacement even if it was off
  --no-power-on          Do not power on the VM after replacement
  --power-timeout SEC    Seconds to wait for power transitions (default: 60)
  --configure-ps2-mouse  Try VMX legacy PS/2 mouse settings before power-on
  --skip-build            Do not run make esxi-vmdk before upload
  --force                 Replace an existing imported VMDK on the datastore
  --dry-run               Print commands without running them
  -h, --help              Show this help

Examples:
  ESXI_HOST=10.10.0.13 tools/deploy_esxi.sh --force
  ESXI_PASSWORD='...' tools/deploy_esxi.sh --host 10.10.0.13 --force
  tools/deploy_esxi.sh --host 10.10.0.13 --force --attach-and-reboot
  tools/deploy_esxi.sh --host 10.10.0.13 --display-backend vga --datastore datastore1
EOF
}

die() {
    printf 'deploy_esxi: %s\n' "$*" >&2
    exit 1
}

run() {
    if [ "$DRY_RUN" -eq 1 ]; then
        printf '+'
        printf ' %q' "$@"
        printf '\n'
        return 0
    fi
    "$@"
}

print_cmd() {
    printf '+'
    printf ' %q' "$@"
    printf '\n'
}

ssh_run() {
    if [ "$USE_SSHPASS" -eq 1 ]; then
        if [ "$DRY_RUN" -eq 1 ]; then
            print_cmd env SSHPASS=REDACTED sshpass -e ssh "${SSH_OPTS[@]}" "$@"
            return 0
        fi
        env SSHPASS="$ESXI_PASSWORD_VALUE" sshpass -e ssh "${SSH_OPTS[@]}" "$@"
        return $?
    fi
    run ssh "${SSH_OPTS[@]}" "$@"
}

scp_run() {
    if [ "$USE_SSHPASS" -eq 1 ]; then
        if [ "$DRY_RUN" -eq 1 ]; then
            print_cmd env SSHPASS=REDACTED sshpass -e scp "${SSH_OPTS[@]}" "$@"
            return 0
        fi
        env SSHPASS="$ESXI_PASSWORD_VALUE" sshpass -e scp "${SSH_OPTS[@]}" "$@"
        return $?
    fi
    run scp "${SSH_OPTS[@]}" "$@"
}

sq() {
    case $1 in
        *$'\n'*|*$'\r'*)
            die "remote paths must not contain newlines"
            ;;
    esac
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

require_tool() {
    command -v "$1" >/dev/null 2>&1 || die "missing required tool: $1"
}

repo_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

ESXI_HOST="${ESXI_HOST:-}"
ESXI_USER="${ESXI_USER:-root}"
ESXI_PASSWORD_ENV="${ESXI_PASSWORD_ENV:-ESXI_PASSWORD}"
ESXI_DATASTORE="${ESXI_DATASTORE:-datastore1}"
ESXI_VM_DIR="${ESXI_VM_DIR:-SmallOS}"
ESXI_VM_NAME="${ESXI_VM_NAME:-}"
DISPLAY_BACKEND="${DISPLAY_BACKEND:-auto}"
ESXI_VMDK_SIZE="${ESXI_VMDK_SIZE-}"
LOCAL_VMDK=""
REMOTE_NAME="${ESXI_REMOTE_NAME:-smallos-esxi-vmfs.vmdk}"
UPLOAD_NAME="${ESXI_UPLOAD_NAME:-smallos-esxi-upload.vmdk}"
VM_DISK_CONTROLLER="${ESXI_VM_DISK_CONTROLLER:-0}"
VM_DISK_UNIT="${ESXI_VM_DISK_UNIT:-0}"
VM_CONTROLLER_TYPE="${ESXI_VM_CONTROLLER_TYPE:-ide}"
POWER_TIMEOUT="${ESXI_POWER_TIMEOUT:-60}"
CONFIGURE_PS2_MOUSE="${ESXI_CONFIGURE_PS2_MOUSE:-0}"
REPLACE_VM_DISK="${ESXI_REPLACE_VM_DISK:-0}"
POWER_ON_MODE="${ESXI_POWER_ON_MODE:-restore}"
SKIP_BUILD=0
FORCE=0
DRY_RUN=0

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
        --display-backend)
            [ "$#" -ge 2 ] || die "--display-backend requires a value"
            DISPLAY_BACKEND="$2"
            shift 2
            ;;
        --size)
            [ "$#" -ge 2 ] || die "--size requires a value"
            ESXI_VMDK_SIZE="$2"
            shift 2
            ;;
        --local-vmdk)
            [ "$#" -ge 2 ] || die "--local-vmdk requires a value"
            LOCAL_VMDK="$2"
            SKIP_BUILD=1
            shift 2
            ;;
        --remote-name)
            [ "$#" -ge 2 ] || die "--remote-name requires a value"
            REMOTE_NAME="$2"
            shift 2
            ;;
        --upload-name)
            [ "$#" -ge 2 ] || die "--upload-name requires a value"
            UPLOAD_NAME="$2"
            shift 2
            ;;
        --vm-name)
            [ "$#" -ge 2 ] || die "--vm-name requires a value"
            ESXI_VM_NAME="$2"
            shift 2
            ;;
        --replace-vm-disk)
            REPLACE_VM_DISK=1
            shift
            ;;
        --attach-and-reboot)
            REPLACE_VM_DISK=1
            POWER_ON_MODE=always
            shift
            ;;
        --disk-controller)
            [ "$#" -ge 2 ] || die "--disk-controller requires a value"
            VM_DISK_CONTROLLER="$2"
            shift 2
            ;;
        --disk-unit)
            [ "$#" -ge 2 ] || die "--disk-unit requires a value"
            VM_DISK_UNIT="$2"
            shift 2
            ;;
        --controller-type)
            [ "$#" -ge 2 ] || die "--controller-type requires a value"
            VM_CONTROLLER_TYPE="$2"
            shift 2
            ;;
        --power-on)
            POWER_ON_MODE=always
            shift
            ;;
        --no-power-on)
            POWER_ON_MODE=never
            shift
            ;;
        --power-timeout)
            [ "$#" -ge 2 ] || die "--power-timeout requires a value"
            POWER_TIMEOUT="$2"
            shift 2
            ;;
        --configure-ps2-mouse)
            CONFIGURE_PS2_MOUSE=1
            shift
            ;;
        --skip-build)
            SKIP_BUILD=1
            shift
            ;;
        --force)
            FORCE=1
            shift
            ;;
        --dry-run)
            DRY_RUN=1
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

case "$REMOTE_NAME" in
    ""|*/*) die "--remote-name must be a VMDK filename, not a path" ;;
    *.vmdk) ;;
    *) die "--remote-name must end in .vmdk" ;;
esac

case "$UPLOAD_NAME" in
    ""|*/*) die "--upload-name must be a VMDK filename, not a path" ;;
    *.vmdk) ;;
    *) die "--upload-name must end in .vmdk" ;;
esac

case "$REPLACE_VM_DISK" in
    0|1) ;;
    *) die "ESXI_REPLACE_VM_DISK must be 0 or 1" ;;
esac

case "$POWER_ON_MODE" in
    restore|always|never) ;;
    *) die "ESXI_POWER_ON_MODE must be restore, always, or never" ;;
esac

case "$VM_DISK_CONTROLLER" in
    ''|*[!0-9]*) die "--disk-controller must be a non-negative integer" ;;
esac

case "$VM_DISK_UNIT" in
    ''|*[!0-9]*) die "--disk-unit must be a non-negative integer" ;;
esac

case "$POWER_TIMEOUT" in
    ''|*[!0-9]*) die "--power-timeout must be a non-negative integer" ;;
esac

if [ "$REPLACE_VM_DISK" -eq 1 ] && [ -z "$ESXI_VM_NAME" ]; then
    ESXI_VM_NAME="$ESXI_VM_DIR"
fi

require_tool ssh
require_tool scp
if [ "$SKIP_BUILD" -eq 0 ]; then
    require_tool make
fi

ESXI_PASSWORD_VALUE="${!ESXI_PASSWORD_ENV:-}"
USE_SSHPASS=0
if [ -n "$ESXI_PASSWORD_VALUE" ]; then
    require_tool sshpass
    USE_SSHPASS=1
fi
SSH_OPTS=(-o StrictHostKeyChecking=accept-new)

if [ "$SKIP_BUILD" -eq 0 ]; then
    build_args=(-C "$repo_root" esxi-vmdk DISPLAY_BACKEND="$DISPLAY_BACKEND")
    if [ -n "$ESXI_VMDK_SIZE" ]; then
        build_args+=(ESXI_VMDK_SIZE="$ESXI_VMDK_SIZE")
    fi
    run make "${build_args[@]}"
fi

if [ -z "$LOCAL_VMDK" ]; then
    LOCAL_VMDK="$repo_root/build/img/esxi/${DISPLAY_BACKEND}-serial/smallos-esxi.vmdk"
fi

[ -f "$LOCAL_VMDK" ] || die "local VMDK not found: $LOCAL_VMDK"

remote_dir="/vmfs/volumes/${ESXI_DATASTORE}/${ESXI_VM_DIR}"
remote_upload="${remote_dir}/${UPLOAD_NAME}"
remote_import="${remote_dir}/${REMOTE_NAME}"
remote_base="${REMOTE_NAME%.vmdk}"
remote_flat="${remote_dir}/${remote_base}-flat.vmdk"
remote_disk_ref="[${ESXI_DATASTORE}] ${ESXI_VM_DIR}/${REMOTE_NAME}"
remote_disk_add_path="$remote_import"
remote_user_host="${ESXI_USER}@${ESXI_HOST}"

printf 'ESXi host:       %s\n' "$remote_user_host"
printf 'Datastore dir:   %s\n' "$remote_dir"
printf 'Local VMDK:      %s\n' "$LOCAL_VMDK"
printf 'Upload VMDK:     %s\n' "$remote_upload"
printf 'Imported VMDK:   %s\n' "$remote_import"
if [ "$REPLACE_VM_DISK" -eq 1 ]; then
    printf 'VM:              %s\n' "$ESXI_VM_NAME"
    printf 'VM disk slot:    %s %s:%s\n' "$VM_CONTROLLER_TYPE" "$VM_DISK_CONTROLLER" "$VM_DISK_UNIT"
    if [ "$CONFIGURE_PS2_MOUSE" -eq 1 ]; then
        printf 'VM input:        PS/2 mouse\n'
    fi
fi

ssh_run "$remote_user_host" "mkdir -p $(sq "$remote_dir")"
scp_run "$LOCAL_VMDK" "${remote_user_host}:${remote_upload}"

remote_script=$(cat <<EOF
set -eu
cd $(sq "$remote_dir")

find_vmid_by_name() {
    vim-cmd vmsvc/getallvms | awk -v want="\$1" '
        NR > 1 {
            line = \$0
            sub(/^[[:space:]]+/, "", line)
            vmid = line
            sub(/[[:space:]].*/, "", vmid)
            sub(/^[0-9]+[[:space:]]+/, "", line)
            idx = index(line, " [")
            if (idx > 0) {
                name = substr(line, 1, idx - 1)
                sub(/[[:space:]]+\$/, "", name)
                if (name == want) {
                    print vmid
                }
            }
        }
    '
}

power_state() {
    vim-cmd vmsvc/power.getstate "\$1" | tail -n 1
}

wait_power_state() {
    vmid="\$1"
    want="\$2"
    timeout="\$3"
    elapsed=0
    while [ "\$elapsed" -le "\$timeout" ]; do
        state="\$(power_state "\$vmid")"
        if [ "\$state" = "\$want" ]; then
            return 0
        fi
        sleep 1
        elapsed=\$((elapsed + 1))
    done
    echo "Timed out waiting for VM \$vmid to reach power state: \$want" >&2
    return 1
}

answer_vm_message_default() {
    vmid="\$1"
    msg="\$(vim-cmd vmsvc/message "\$vmid" 2>/dev/null || true)"
    case "\$msg" in
        *"Virtual machine message "*)
            msg_id="\$(printf '%s\n' "\$msg" | awk '/Virtual machine message/ { gsub(":", "", \$4); print \$4; exit }')"
            if [ -n "\$msg_id" ]; then
                vim-cmd vmsvc/message "\$vmid" "\$msg_id" 0 >/dev/null 2>&1 || true
            fi
            ;;
    esac
}

power_on_vm() {
    vmid="\$1"
    timeout="\$2"
    elapsed=0

    vim-cmd vmsvc/power.on "\$vmid" &
    power_pid=\$!
    while [ "\$elapsed" -le "\$timeout" ]; do
        answer_vm_message_default "\$vmid"
        state="\$(power_state "\$vmid")"
        if [ "\$state" = "Powered on" ]; then
            wait "\$power_pid" >/dev/null 2>&1 || true
            return 0
        fi
        sleep 1
        elapsed=\$((elapsed + 1))
    done

    wait "\$power_pid" >/dev/null 2>&1 || true
    wait_power_state "\$vmid" "Powered on" "\$timeout"
}

set_vmx_key() {
    vmx="\$1"
    key="\$2"
    value="\$3"
    tmp="\${vmx}.tmp.\$\$"

    if grep -q "^\\\$key[[:space:]]*=" "\$vmx"; then
        awk -v key="\$key" -v value="\$value" '
            BEGIN { done = 0 }
            \$0 ~ "^" key "[[:space:]]*=" {
                print key " = \\"" value "\\""
                done = 1
                next
            }
            { print }
            END {
                if (!done) {
                    print key " = \\"" value "\\""
                }
            }
        ' "\$vmx" > "\$tmp"
    else
        cp "\$vmx" "\$tmp"
        printf '%s = "%s"\n' "\$key" "\$value" >> "\$tmp"
    fi
    mv "\$tmp" "\$vmx"
}

configure_ps2_mouse() {
    vmx_count="\$(find . -maxdepth 1 -name '*.vmx' | sed '/^$/d' | awk 'END { print NR }')"
    if [ "\$vmx_count" != "1" ]; then
        echo "Skipping PS/2 mouse VMX config: expected one .vmx in \$PWD, found \$vmx_count" >&2
        return 0
    fi

    vmx="\$(find . -maxdepth 1 -name '*.vmx' | sed -n '1p')"
    set_vmx_key "\$vmx" mouse.present TRUE
    set_vmx_key "\$vmx" mouse.vusb.enable FALSE
    set_vmx_key "\$vmx" vmmouse.present FALSE
    echo "VMX PS/2 mouse settings updated: \$vmx"
}

vmid=""
original_power_state=""
if [ "$REPLACE_VM_DISK" -eq 1 ]; then
    vmids="\$(find_vmid_by_name $(sq "$ESXI_VM_NAME"))"
    vmid_count="\$(printf '%s\n' "\$vmids" | sed '/^$/d' | awk 'END { print NR }')"
    if [ "\$vmid_count" != "1" ]; then
        echo "Expected exactly one VM named $(sq "$ESXI_VM_NAME"), found \$vmid_count" >&2
        vim-cmd vmsvc/getallvms >&2
        exit 2
    fi
    vmid="\$(printf '%s\n' "\$vmids" | sed -n '1p')"
    original_power_state="\$(power_state "\$vmid")"
    echo "VM $(sq "$ESXI_VM_NAME") has id \$vmid and is \$original_power_state"

    if [ -e $(sq "$REMOTE_NAME") ] || [ -e $(sq "${remote_base}-flat.vmdk") ]; then
        if [ "$FORCE" -ne 1 ]; then
            echo "Refusing to replace existing imported VMDK: $remote_import" >&2
            echo "Re-run with --force to replace it." >&2
            exit 2
        fi
    fi

    if [ "\$original_power_state" = "Powered on" ]; then
        vim-cmd vmsvc/power.off "\$vmid"
        wait_power_state "\$vmid" "Powered off" "$POWER_TIMEOUT"
    fi

    vim-cmd vmsvc/device.diskremove "\$vmid" "$VM_DISK_CONTROLLER" "$VM_DISK_UNIT" false "$VM_CONTROLLER_TYPE"
elif [ "$FORCE" -ne 1 ] && { [ -e $(sq "$REMOTE_NAME") ] || [ -e $(sq "${remote_base}-flat.vmdk") ]; }; then
    echo "Refusing to replace existing imported VMDK: $remote_import" >&2
    echo "Re-run with --force to replace it." >&2
    exit 2
fi

if [ "$FORCE" -eq 1 ]; then
    rm -f $(sq "$REMOTE_NAME") $(sq "${remote_base}-flat.vmdk")
fi

vmkfstools -i $(sq "$UPLOAD_NAME") $(sq "$REMOTE_NAME") -d thin
ls -lh $(sq "$UPLOAD_NAME") $(sq "$REMOTE_NAME") $(sq "${remote_base}-flat.vmdk") 2>/dev/null || true

if [ "$REPLACE_VM_DISK" -eq 1 ]; then
    vim-cmd vmsvc/device.diskaddexisting "\$vmid" $(sq "$remote_disk_add_path") "$VM_DISK_CONTROLLER" "$VM_DISK_UNIT" "$VM_CONTROLLER_TYPE"
    if [ "$CONFIGURE_PS2_MOUSE" -eq 1 ]; then
        configure_ps2_mouse
    fi
    vim-cmd vmsvc/reload "\$vmid"
    case "$POWER_ON_MODE" in
        always)
            power_on_vm "\$vmid" "$POWER_TIMEOUT"
            ;;
        restore)
            if [ "\$original_power_state" = "Powered on" ]; then
                power_on_vm "\$vmid" "$POWER_TIMEOUT"
            fi
            ;;
        never)
            ;;
    esac
    echo "VM disk attached: $(sq "$remote_disk_ref")"
    echo "VM power state: \$(power_state "\$vmid")"
fi
EOF
)

remote_script_file="$(mktemp)"
remote_script_name=".smallos-deploy-$$.sh"
remote_script_remote="${remote_dir}/${remote_script_name}"
printf '%s\n' "$remote_script" > "$remote_script_file"
scp_run "$remote_script_file" "${remote_user_host}:${remote_script_remote}"
rm -f "$remote_script_file"
ssh_run "$remote_user_host" "chmod +x $(sq "$remote_script_remote"); sh $(sq "$remote_script_remote"); rc=\$?; rm -f $(sq "$remote_script_remote"); exit \$rc"

if [ "$REPLACE_VM_DISK" -eq 1 ]; then
    printf '\nDeploy complete. VM disk replaced and VM lifecycle handled:\n'
    printf '  disk: [%s] %s/%s\n' "$ESXI_DATASTORE" "$ESXI_VM_DIR" "$REMOTE_NAME"
    printf 'VM updated: %s (%s %s:%s)\n' "$ESXI_VM_NAME" "$VM_CONTROLLER_TYPE" "$VM_DISK_CONTROLLER" "$VM_DISK_UNIT"
else
    printf '\nDeploy complete. Attach this disk to the VM as IDE:\n'
    printf '  [%s] %s/%s\n' "$ESXI_DATASTORE" "$ESXI_VM_DIR" "$REMOTE_NAME"
fi
