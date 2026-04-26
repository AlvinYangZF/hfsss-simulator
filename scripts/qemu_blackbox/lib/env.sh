#!/bin/bash

set -euo pipefail

LIB_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
# shellcheck source=common.sh
. "$LIB_DIR/common.sh"

hfsss_blackbox_init_defaults() {
    local host_os host_arch

    host_os="$(uname -s)"
    host_arch="$(uname -m)"

    HFSSS_PROJECT_DIR="${HFSSS_PROJECT_DIR:-$(cd "$LIB_DIR/../../.." && pwd)}"
    HFSSS_BLACKBOX_ROOT="${HFSSS_BLACKBOX_ROOT:-$(cd "$LIB_DIR/.." && pwd)}"
    HFSSS_NBD_PORT="${HFSSS_NBD_PORT:-10820}"
    HFSSS_SSH_PORT="${HFSSS_SSH_PORT:-10022}"
    HFSSS_NBD_SIZE_MB="${HFSSS_NBD_SIZE_MB:-512}"
    HFSSS_QEMU_MEM="${HFSSS_QEMU_MEM:-2G}"
    HFSSS_QEMU_SMP="${HFSSS_QEMU_SMP:-2}"
    HFSSS_NBD_MODE="${HFSSS_NBD_MODE:-mt}"
    HFSSS_GUEST_NVME_DEV="${HFSSS_GUEST_NVME_DEV:-/dev/nvme0n1}"
    HFSSS_GUEST_NVME_CTRL="${HFSSS_GUEST_NVME_CTRL:-/dev/nvme0}"
    # Persistent SSH key location. Older default lived under /tmp/, which
    # macOS wipes on reboot, so the runner re-generated a fresh key on
    # first run after every reboot — and the per-machine cidata.iso (which
    # bakes the previous pubkey into the guest's authorized_keys) then no
    # longer authorized it. ~/.ssh/ survives reboots and is the standard
    # spot for user-private SSH material.
    HFSSS_SSH_KEY="${HFSSS_SSH_KEY:-${HOME}/.ssh/hfsss_qemu_key}"
    HFSSS_ARTIFACT_ROOT="${HFSSS_ARTIFACT_ROOT:-$HFSSS_PROJECT_DIR/build/blackbox-tests/$(hfsss_timestamp)}"
    HFSSS_QEMU_CODE_FD="${HFSSS_QEMU_CODE_FD:-/opt/homebrew/share/qemu/edk2-aarch64-code.fd}"
    HFSSS_QEMU_BIN="${HFSSS_QEMU_BIN:-qemu-system-aarch64}"
    HFSSS_QEMU_MACHINE="${HFSSS_QEMU_MACHINE:-virt,gic-version=3}"
    if [ -z "${HFSSS_QEMU_ACCEL+x}" ]; then
        if [ "$host_os" = "Darwin" ] && [ "$host_arch" = "arm64" ]; then
            HFSSS_QEMU_ACCEL="hvf"
        else
            HFSSS_QEMU_ACCEL="tcg"
        fi
    fi
    if [ -z "${HFSSS_QEMU_CPU+x}" ]; then
        if [ "$HFSSS_QEMU_ACCEL" = "hvf" ] && [ "$host_os" = "Darwin" ] && [ "$host_arch" = "arm64" ]; then
            HFSSS_QEMU_CPU="host"
        else
            HFSSS_QEMU_CPU="max"
        fi
    fi
    HFSSS_CASE_TIMEOUT_S="${HFSSS_CASE_TIMEOUT_S:-300}"
    HFSSS_GUEST_DIR="${HFSSS_GUEST_DIR:-}"
    HFSSS_BUILD_NBD="${HFSSS_BUILD_NBD:-0}"
    HFSSS_KEEP_ENV="${HFSSS_KEEP_ENV:-0}"
    HFSSS_REUSE_ENV="${HFSSS_REUSE_ENV:-0}"
    HFSSS_NBD_PID=""
    HFSSS_QEMU_PID=""
    HFSSS_NBD_LOG="$HFSSS_ARTIFACT_ROOT/nbd-server.log"
    HFSSS_QEMU_CONSOLE_LOG="$HFSSS_ARTIFACT_ROOT/qemu-console.log"
}

hfsss_blackbox_list_cases() {
    local case_dir="$HFSSS_BLACKBOX_ROOT/cases"

    find "$case_dir" -type f -name '*.sh' | sort | sed "s#^$case_dir/##"
}

hfsss_blackbox_resolve_guest_dir() {
    if [ -n "$HFSSS_GUEST_DIR" ]; then
        [ -d "$HFSSS_GUEST_DIR" ] || hfsss_die "guest dir does not exist: $HFSSS_GUEST_DIR"
        return
    fi

    if [ -d "$HFSSS_PROJECT_DIR/guest" ]; then
        HFSSS_GUEST_DIR="$HFSSS_PROJECT_DIR/guest"
        return
    fi

    hfsss_die "guest assets not found; pass --guest-dir or set HFSSS_GUEST_DIR"
}

hfsss_blackbox_validate_guest_assets() {
    local required=(
        "$HFSSS_GUEST_DIR/alpine-hfsss.qcow2"
        "$HFSSS_GUEST_DIR/cidata.iso"
        "$HFSSS_GUEST_DIR/ovmf_vars-saved.fd"
        "$HFSSS_QEMU_CODE_FD"
    )
    local path

    for path in "${required[@]}"; do
        [ -f "$path" ] || hfsss_die "required asset missing: $path"
    done
}

hfsss_blackbox_prepare_dirs() {
    mkdir -p "$HFSSS_ARTIFACT_ROOT"
}

hfsss_blackbox_write_manifest() {
    local manifest="$HFSSS_ARTIFACT_ROOT/environment.txt"
    local git_sha="unknown"

    git_sha="$(git -C "$HFSSS_PROJECT_DIR" rev-parse HEAD 2>/dev/null || printf 'unknown')"

    {
        printf 'nbd_port=%s\n' "$HFSSS_NBD_PORT"
        printf 'ssh_port=%s\n' "$HFSSS_SSH_PORT"
        printf 'nbd_mode=%s\n' "$HFSSS_NBD_MODE"
        printf 'nbd_size_mb=%s\n' "$HFSSS_NBD_SIZE_MB"
        printf 'case_timeout_s=%s\n' "$HFSSS_CASE_TIMEOUT_S"
        printf 'guest_nvme_dev=%s\n' "$HFSSS_GUEST_NVME_DEV"
        printf 'guest_nvme_ctrl=%s\n' "$HFSSS_GUEST_NVME_CTRL"
        printf 'qemu_bin=%s\n' "$HFSSS_QEMU_BIN"
        printf 'qemu_machine=%s\n' "$HFSSS_QEMU_MACHINE"
        printf 'qemu_accel=%s\n' "$HFSSS_QEMU_ACCEL"
        printf 'qemu_cpu=%s\n' "$HFSSS_QEMU_CPU"
        printf 'git_sha=%s\n' "$git_sha"
        printf 'artifacts_dir=%s\n' "$HFSSS_ARTIFACT_ROOT"
        if [ -n "${HFSSS_NBD_PID:-}" ]; then
            printf 'nbd_pid=%s\n' "$HFSSS_NBD_PID"
        fi
        if [ -n "${HFSSS_QEMU_PID:-}" ]; then
            printf 'qemu_pid=%s\n' "$HFSSS_QEMU_PID"
        fi
    } >"$manifest"
}

hfsss_blackbox_reserve_ports() {
    local requested_nbd="$HFSSS_NBD_PORT"
    local requested_ssh="$HFSSS_SSH_PORT"

    if hfsss_blackbox_port_in_use "$requested_nbd"; then
        HFSSS_NBD_PORT="$(hfsss_blackbox_find_free_port $((requested_nbd + 1)))" \
            || hfsss_die "unable to find a free NBD port starting from $requested_nbd"
        hfsss_warn "NBD port $requested_nbd is busy; using $HFSSS_NBD_PORT"
    fi

    if hfsss_blackbox_port_in_use "$requested_ssh"; then
        HFSSS_SSH_PORT="$(hfsss_blackbox_find_free_port $((requested_ssh + 1)))" \
            || hfsss_die "unable to find a free SSH port starting from $requested_ssh"
        hfsss_warn "SSH forward port $requested_ssh is busy; using $HFSSS_SSH_PORT"
    fi

    [ "$HFSSS_NBD_PORT" != "$HFSSS_SSH_PORT" ] || hfsss_die "NBD and SSH ports must differ"
}

hfsss_blackbox_prepare_ssh_key() {
    local ssh_dir home_norm legacy_key cidata_iso
    ssh_dir="$(dirname "$HFSSS_SSH_KEY")"

    # Atomic create-with-permissions, only when the directory is missing.
    # We never modify perms on an existing ~/.ssh — it may carry
    # intentional ACLs, group settings, or be NFS-mounted with shared
    # workflows we should not perturb.
    [ -d "$ssh_dir" ] || mkdir -m 700 -p "$ssh_dir"

    # One-shot migration from the legacy /tmp/ default. macOS wipes
    # /tmp on reboot; relocating the key into ~/.ssh/ on first run
    # post-reboot preserves the keypair the existing cidata.iso was
    # built against, so SSH bootstrap keeps working without a rebuild.
    # Trailing slash on $HOME (rare but possible from /etc/passwd or
    # exotic shells) compares as a different string but resolves the
    # same, so normalize before the equality check. Migration only
    # triggers when the caller did not override HFSSS_SSH_KEY.
    home_norm="${HOME%/}"
    legacy_key="/tmp/hfsss_qemu_key"
    if [ "$HFSSS_SSH_KEY" = "${home_norm}/.ssh/hfsss_qemu_key" ] \
       && [ ! -f "$HFSSS_SSH_KEY" ] \
       && [ -f "$legacy_key" ] \
       && [ -f "${legacy_key}.pub" ]; then
        # Both halves of the keypair must move together — an orphan
        # private key with no matching public would silently corrupt
        # downstream auth flows.
        mv "$legacy_key"     "$HFSSS_SSH_KEY"
        mv "${legacy_key}.pub" "${HFSSS_SSH_KEY}.pub"
        chmod 600 "$HFSSS_SSH_KEY"
        chmod 644 "${HFSSS_SSH_KEY}.pub"
        return
    fi

    if [ ! -f "$HFSSS_SSH_KEY" ]; then
        ssh-keygen -t ed25519 -f "$HFSSS_SSH_KEY" -N "" -q
        # If a guest cidata.iso already exists, it almost certainly
        # authorizes the keypair we just lost (e.g. /tmp/ wiped without
        # a legacy keypair to migrate). The runner cannot rebuild the
        # iso from here — that's a separate tool — but it can flag the
        # situation explicitly so the SSH-not-reachable failure has a
        # visible upstream cause instead of looking like a guest hang.
        cidata_iso=""
        if [ -n "${HFSSS_GUEST_DIR:-}" ] && [ -f "$HFSSS_GUEST_DIR/cidata.iso" ]; then
            cidata_iso="$HFSSS_GUEST_DIR/cidata.iso"
        elif [ -f "${HFSSS_PROJECT_DIR:-$PWD}/guest/cidata.iso" ]; then
            cidata_iso="${HFSSS_PROJECT_DIR:-$PWD}/guest/cidata.iso"
        fi
        if [ -n "$cidata_iso" ]; then
            cat >&2 <<EOF
[hfsss-blackbox] WARN: generated a fresh SSH keypair at $HFSSS_SSH_KEY
[hfsss-blackbox]       but $cidata_iso already exists. The previously
[hfsss-blackbox]       authorized pubkey is gone, so SSH auth into the
[hfsss-blackbox]       guest will fail until cidata.iso is rebuilt
[hfsss-blackbox]       against the new pubkey. See
[hfsss-blackbox]       docs/PRE_CHECKIN_STANDARD.md for the rebuild recipe.
EOF
        fi
    fi
}

hfsss_blackbox_prepare_build() {
    if [ "$HFSSS_BUILD_NBD" != "1" ] && [ -x "$HFSSS_PROJECT_DIR/build/bin/hfsss-nbd-server" ]; then
        return
    fi

    if [ ! -x "$HFSSS_PROJECT_DIR/build/bin/hfsss-nbd-server" ] || [ "$HFSSS_BUILD_NBD" = "1" ]; then
        hfsss_log "building hfsss-nbd-server"
        make -C "$HFSSS_PROJECT_DIR" directories build/bin/hfsss-nbd-server
    fi
}

hfsss_blackbox_wait_for_port() {
    local port="$1"
    local attempts="${2:-30}"
    local i

    for i in $(seq 1 "$attempts"); do
        if lsof -i :"$port" -P 2>/dev/null | grep -q LISTEN; then
            return 0
        fi
        sleep 1
    done

    return 1
}

hfsss_blackbox_port_in_use() {
    local port="$1"
    lsof -i :"$port" -P 2>/dev/null | grep -q LISTEN
}

hfsss_blackbox_find_free_port() {
    local start_port="$1"
    local attempts="${2:-50}"
    local port="$start_port"
    local i

    for i in $(seq 1 "$attempts"); do
        if ! hfsss_blackbox_port_in_use "$port"; then
            printf '%s\n' "$port"
            return 0
        fi
        port=$((port + 1))
    done

    return 1
}

hfsss_blackbox_log_has() {
    local file="$1"
    local pattern="$2"

    [ -f "$file" ] || return 1
    grep -q "$pattern" "$file"
}

hfsss_blackbox_wait_for_nbd_ready() {
    local pid="$1"
    local log_file="$2"
    local port="$3"
    local attempts="${4:-30}"
    local i

    for i in $(seq 1 "$attempts"); do
        if ! kill -0 "$pid" 2>/dev/null; then
            hfsss_die "NBD server exited early; see $log_file"
        fi

        if hfsss_blackbox_wait_for_port "$port" 1 && \
           hfsss_blackbox_log_has "$log_file" "Waiting for NBD client"; then
            return 0
        fi

        sleep 1
    done

    hfsss_die "NBD server did not become ready; see $log_file"
}

hfsss_blackbox_ssh() {
    ssh -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o ConnectTimeout=5 \
        -i "$HFSSS_SSH_KEY" \
        -p "$HFSSS_SSH_PORT" \
        root@127.0.0.1 "$@"
}

hfsss_guest_run() {
    local cmd="$1"
    hfsss_blackbox_ssh "sh -lc $(hfsss_quote_remote "$cmd")"
}

hfsss_guest_capture() {
    local stdout_file="$1"
    local stderr_file="$2"
    local cmd="$3"

    hfsss_guest_run "$cmd" >"$stdout_file" 2>"$stderr_file"
}

hfsss_guest_has_tool() {
    local tool="$1"
    hfsss_guest_run "command -v $tool >/dev/null 2>&1"
}

hfsss_guest_wait_for_ssh() {
    local attempts="${1:-90}"
    local required_successes="${2:-3}"
    local i
    local successes=0

    for i in $(seq 1 "$attempts"); do
        if hfsss_guest_run "true" >/dev/null 2>&1; then
            successes=$((successes + 1))
            if [ "$successes" -ge "$required_successes" ]; then
                return 0
            fi
        else
            successes=0
        fi
        sleep 1
    done

    return 1
}

hfsss_blackbox_wait_for_guest_ready() {
    local qemu_pid="$1"
    local attempts="${2:-90}"
    local i
    local successes=0

    for i in $(seq 1 "$attempts"); do
        if ! kill -0 "$qemu_pid" 2>/dev/null; then
            hfsss_die "QEMU exited early; see $HFSSS_ARTIFACT_ROOT/qemu-launch.log and $HFSSS_QEMU_CONSOLE_LOG"
        fi

        if hfsss_guest_run "true" >/dev/null 2>&1 && \
           hfsss_guest_run "test -b $HFSSS_GUEST_NVME_DEV" >/dev/null 2>&1; then
            successes=$((successes + 1))
            if [ "$successes" -ge 3 ]; then
                return 0
            fi
        else
            successes=0
        fi

        sleep 1
    done

    hfsss_die "QEMU guest did not become reachable over SSH; see $HFSSS_ARTIFACT_ROOT/qemu-launch.log and $HFSSS_QEMU_CONSOLE_LOG"
}

hfsss_blackbox_nbd_mode_flag() {
    case "$HFSSS_NBD_MODE" in
        sync) printf '' ;;
        mt) printf '%s' '-m' ;;
        async) printf '%s' '-a' ;;
        *) hfsss_die "unsupported NBD mode: $HFSSS_NBD_MODE" ;;
    esac
}

hfsss_blackbox_start_env() {
    local nbd_flag
    local ovmf_run="$HFSSS_ARTIFACT_ROOT/ovmf_vars-run.fd"

    hfsss_require_cmd "$HFSSS_QEMU_BIN" ssh lsof make
    hfsss_blackbox_resolve_guest_dir
    hfsss_blackbox_validate_guest_assets
    hfsss_blackbox_prepare_dirs
    hfsss_blackbox_prepare_ssh_key
    hfsss_blackbox_prepare_build
    hfsss_blackbox_reserve_ports

    cp "$HFSSS_GUEST_DIR/ovmf_vars-saved.fd" "$ovmf_run"

    nbd_flag="$(hfsss_blackbox_nbd_mode_flag)"
    local nbd_bin="${HFSSS_NBD_BIN:-$HFSSS_PROJECT_DIR/build/bin/hfsss-nbd-server}"
    hfsss_log "starting hfsss-nbd-server ($HFSSS_NBD_MODE mode, bin=$nbd_bin)"
    declare -a nbd_argv=(-p "$HFSSS_NBD_PORT" -s "$HFSSS_NBD_SIZE_MB")
    [ -n "$nbd_flag" ] && nbd_argv=("$nbd_flag" "${nbd_argv[@]}")
    [ -n "${HFSSS_NBD_PROFILE:-}" ] && nbd_argv+=(-P "$HFSSS_NBD_PROFILE")
    "$nbd_bin" "${nbd_argv[@]}" >"$HFSSS_NBD_LOG" 2>&1 &
    HFSSS_NBD_PID=$!
    hfsss_blackbox_write_manifest

    hfsss_blackbox_wait_for_nbd_ready "$HFSSS_NBD_PID" "$HFSSS_NBD_LOG" "$HFSSS_NBD_PORT" 30

    hfsss_log "starting QEMU guest"
    "$HFSSS_QEMU_BIN" \
        -M "$HFSSS_QEMU_MACHINE" -accel "$HFSSS_QEMU_ACCEL" -cpu "$HFSSS_QEMU_CPU" \
        -m "$HFSSS_QEMU_MEM" -smp "$HFSSS_QEMU_SMP" \
        -drive if=pflash,format=raw,file="$HFSSS_QEMU_CODE_FD",readonly=on \
        -drive if=pflash,format=raw,file="$ovmf_run" \
        -drive file="$HFSSS_GUEST_DIR/alpine-hfsss.qcow2",if=virtio,format=qcow2,snapshot=on \
        -drive file="$HFSSS_GUEST_DIR/cidata.iso",if=virtio,media=cdrom \
        -drive "driver=nbd,server.type=inet,server.host=127.0.0.1,server.port=$HFSSS_NBD_PORT,if=none,id=nvm0,discard=unmap" \
        -device nvme,serial=HFSSS0001,drive=nvm0 \
        -netdev "user,id=net0,hostfwd=tcp::${HFSSS_SSH_PORT}-:22" \
        -device virtio-net-pci,netdev=net0 \
        -serial "file:$HFSSS_QEMU_CONSOLE_LOG" \
        -display none >"$HFSSS_ARTIFACT_ROOT/qemu-launch.log" 2>&1 &
    HFSSS_QEMU_PID=$!
    hfsss_blackbox_write_manifest

    hfsss_blackbox_wait_for_guest_ready "$HFSSS_QEMU_PID" 90
    hfsss_guest_run "test -b $HFSSS_GUEST_NVME_DEV" >/dev/null \
        || hfsss_die "guest block device not found: $HFSSS_GUEST_NVME_DEV"
}

hfsss_blackbox_verify_existing_env() {
    hfsss_require_cmd ssh
    hfsss_blackbox_prepare_ssh_key
    hfsss_guest_wait_for_ssh 10 3 || hfsss_die "existing guest is not reachable over SSH"
    hfsss_guest_run "test -b $HFSSS_GUEST_NVME_DEV" >/dev/null \
        || hfsss_die "guest block device not found: $HFSSS_GUEST_NVME_DEV"
}

hfsss_blackbox_stop_env() {
    if [ -n "${HFSSS_QEMU_PID:-}" ]; then
        kill "$HFSSS_QEMU_PID" 2>/dev/null || true
        wait "$HFSSS_QEMU_PID" 2>/dev/null || true
        HFSSS_QEMU_PID=""
    fi

    if [ -n "${HFSSS_NBD_PID:-}" ]; then
        kill "$HFSSS_NBD_PID" 2>/dev/null || true
        wait "$HFSSS_NBD_PID" 2>/dev/null || true
        HFSSS_NBD_PID=""
    fi
}

hfsss_blackbox_cleanup() {
    if [ "${HFSSS_REUSE_ENV:-0}" = "1" ]; then
        return
    fi

    if [ "${HFSSS_KEEP_ENV:-0}" != "1" ]; then
        hfsss_blackbox_stop_env
    else
        hfsss_warn "keeping QEMU/NBD environment running by request"
    fi
}

hfsss_blackbox_capture_guest_state() {
    local out_dir="$1"

    mkdir -p "$out_dir"
    hfsss_guest_capture "$out_dir/guest-dmesg.txt" "$out_dir/guest-dmesg.stderr.txt" "dmesg" || true

    if hfsss_guest_has_tool nvme; then
        hfsss_guest_capture \
            "$out_dir/nvme-smart-log.txt" \
            "$out_dir/nvme-smart-log.stderr.txt" \
            "nvme smart-log $HFSSS_GUEST_NVME_CTRL" || true
    fi
}
