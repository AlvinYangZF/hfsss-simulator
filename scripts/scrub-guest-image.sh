#!/bin/bash
# Produce a privacy-clean copy of guest/alpine-hfsss.qcow2 ready for
# distribution as a release artifact. Strips per-machine SSH host keys,
# machine-id, shell histories, log files, cloud-init state, APK cache,
# and any authorized_keys content baked into user homes.
#
# Approach: boot the qcow2 in QEMU non-snapshot mode (so writes persist),
# SSH in via the existing cidata.iso bootstrap, run the scrub from inside
# the guest, request a clean poweroff so the filesystem flushes, then
# compress the result with qemu-img convert.
#
# Output: guest/alpine-hfsss-clean.qcow2

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

GUEST="${HFSSS_GUEST_DIR:-$PROJECT_DIR/guest}"
SRC="$GUEST/alpine-hfsss.qcow2"
WORK="$GUEST/alpine-hfsss-clean-work.qcow2"
DST="$GUEST/alpine-hfsss-clean.qcow2"

[ -f "$SRC" ]                       || { echo "ERROR: $SRC missing"; exit 1; }
[ -f "$GUEST/cidata.iso" ]          || { echo "ERROR: $GUEST/cidata.iso missing"; exit 1; }
[ -f "$GUEST/ovmf_vars-saved.fd" ]  || { echo "ERROR: $GUEST/ovmf_vars-saved.fd missing"; exit 1; }

SSH_KEY="${HFSSS_SSH_KEY:-${HOME}/.ssh/hfsss_qemu_key}"
[ -f "$SSH_KEY" ] || { echo "ERROR: SSH key $SSH_KEY missing — run a make qemu-blackbox once first to generate"; exit 1; }

CODE_FD="${HFSSS_QEMU_CODE_FD:-/opt/homebrew/share/qemu/edk2-aarch64-code.fd}"
[ -f "$CODE_FD" ] || { echo "ERROR: UEFI code firmware missing at $CODE_FD"; exit 1; }

ACCEL="${HFSSS_QEMU_ACCEL:-}"
if [ -z "$ACCEL" ]; then
    if [ "$(uname -s)" = "Darwin" ] && [ "$(uname -m)" = "arm64" ]; then
        ACCEL="hvf"
    else
        ACCEL="tcg"
    fi
fi
CPU="${HFSSS_QEMU_CPU:-host}"

echo "[scrub] copying $SRC -> $WORK"
cp "$SRC" "$WORK"
OVMF_RUN="$(mktemp -t scrub-ovmf-XXXX).fd"
cp "$GUEST/ovmf_vars-saved.fd" "$OVMF_RUN"

SSH_PORT=12022
while lsof -iTCP:"$SSH_PORT" -sTCP:LISTEN -P 2>/dev/null | grep -q LISTEN; do
    SSH_PORT=$((SSH_PORT + 1))
done
echo "[scrub] using SSH port $SSH_PORT"

QEMU_LOG="$(mktemp -t scrub-qemu-XXXX).log"
QEMU_CONSOLE="$(mktemp -t scrub-console-XXXX).log"
# Use explicit -drive if=none + -device with bootindex=1 to force boot
# from the qcow2 even though the OVMF saved-vars boot order may point at
# a PCI slot that depends on the runner's full device set (which includes
# an NBD-backed NVMe at runtime). The scrub launcher does not include the
# NVMe device, so without bootindex the firmware drops into the EFI shell.
qemu-system-aarch64 \
    -M virt,gic-version=3 -accel "$ACCEL" -cpu "$CPU" \
    -m 2G -smp 2 \
    -drive if=pflash,format=raw,file="$CODE_FD",readonly=on \
    -drive if=pflash,format=raw,file="$OVMF_RUN" \
    -drive file="$WORK",if=none,id=hd0,format=qcow2 \
    -device virtio-blk-pci,drive=hd0,bootindex=1 \
    -drive file="$GUEST/cidata.iso",if=virtio,media=cdrom \
    -netdev user,id=net0,hostfwd=tcp::"${SSH_PORT}"-:22 \
    -device virtio-net-pci,netdev=net0 \
    -display none \
    -serial "file:$QEMU_CONSOLE" \
    >"$QEMU_LOG" 2>&1 &
QEMU_PID=$!
echo "[scrub] QEMU PID=$QEMU_PID"

SCRUB_OK=0
cleanup_qemu() {
    if kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "[scrub] killing QEMU PID=$QEMU_PID"
        kill "$QEMU_PID" 2>/dev/null || true
        wait "$QEMU_PID" 2>/dev/null || true
    fi
}
cleanup_on_exit() {
    cleanup_qemu
    if [ "$SCRUB_OK" = "1" ]; then
        rm -f "$OVMF_RUN" "$QEMU_LOG" "$QEMU_CONSOLE" "$WORK"
    else
        echo "[scrub] non-success exit — leaving artifacts for inspection:"
        echo "    qemu-launch.log:   $QEMU_LOG"
        echo "    qemu-console.log:  $QEMU_CONSOLE"
        echo "    work qcow2:        $WORK"
        echo "    ovmf vars copy:    $OVMF_RUN"
    fi
}
trap cleanup_on_exit EXIT

ssh_run() {
    ssh -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR \
        -o ConnectTimeout=5 \
        -i "$SSH_KEY" \
        -p "$SSH_PORT" \
        root@127.0.0.1 "$@"
}

SSH_WAIT_S="${SSH_WAIT_S:-240}"
echo "[scrub] waiting for SSH (up to ${SSH_WAIT_S} s; non-snapshot boots are slower)..."
ready=0
for i in $(seq 1 "$SSH_WAIT_S"); do
    if ssh_run true >/dev/null 2>&1; then
        ready=1
        echo "[scrub] SSH ready after ${i} s"
        break
    fi
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        echo "[scrub] QEMU exited before SSH became ready; launch log:"
        cat "$QEMU_LOG"
        exit 1
    fi
    if [ $((i % 30)) -eq 0 ]; then
        echo "[scrub]   still waiting at ${i} s..."
    fi
    sleep 1
done
if [ "$ready" != "1" ]; then
    echo "[scrub] SSH never became ready; launch log:"
    cat "$QEMU_LOG"
    exit 1
fi

echo "[scrub] running in-guest scrub commands..."
# Note: best-effort semantics — every individual scrub step is allowed
# to fail (file may not exist on this image) without aborting the run.
# A genuine fatal (sshd died, fs read-only, OOM) would surface on the
# poweroff sync step or the qemu-img convert downstream.
ssh_run 'sh -s' <<'SCRUB'
echo "  - removing /etc/ssh/ssh_host_*"
rm -f /etc/ssh/ssh_host_*key /etc/ssh/ssh_host_*key.pub /etc/ssh/ssh_host_*key-cert.pub 2>/dev/null
echo "  - clearing /etc/machine-id"
[ -f /etc/machine-id ] && : > /etc/machine-id
echo "  - removing shell histories"
rm -f /root/.bash_history /root/.ash_history /root/.sh_history 2>/dev/null
rm -f /home/alpine/.bash_history /home/alpine/.ash_history /home/alpine/.sh_history 2>/dev/null
echo "  - truncating /var/log files"
find /var/log -type f -exec sh -c ': > "$1"' _ {} \; 2>/dev/null
echo "  - removing cloud-init state"
rm -rf /var/lib/cloud/* 2>/dev/null
echo "  - clearing APK cache"
rm -rf /var/cache/apk/* 2>/dev/null
echo "  - removing baked authorized_keys"
rm -f /home/alpine/.ssh/authorized_keys /root/.ssh/authorized_keys 2>/dev/null
echo "  - clearing /tmp"
rm -rf /tmp/* /tmp/.??* 2>/dev/null
echo "  - resetting /etc/resolv.conf"
: > /etc/resolv.conf
echo "  - flushing"
sync
echo "scrub complete; powering off"
# Trigger poweroff from within the same SSH session that ran the scrub.
# A second SSH after this point would fail because we just removed
# /root/.ssh/authorized_keys, so we cannot rely on a follow-up call.
nohup sh -c "sync; sleep 1; poweroff" >/dev/null 2>&1 &
sleep 1
SCRUB

echo "[scrub] waiting for guest to halt (up to 60 s)..."
halted=0
for _ in $(seq 1 60); do
    if ! kill -0 "$QEMU_PID" 2>/dev/null; then
        halted=1
        break
    fi
    sleep 1
done
if [ "$halted" != "1" ]; then
    echo "[scrub] guest did not halt cleanly; sending SIGTERM"
    kill "$QEMU_PID" 2>/dev/null || true
    wait "$QEMU_PID" 2>/dev/null || true
fi

echo "[scrub] compressing -> $DST"
qemu-img convert -O qcow2 -c -p "$WORK" "$DST"

echo
echo "[scrub] result:"
qemu-img info "$DST" | sed 's/^/    /'
ls -lh "$DST" | sed 's/^/    /'
echo
SCRUB_OK=1
echo "[scrub] DONE"
