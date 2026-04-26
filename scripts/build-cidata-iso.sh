#!/bin/bash
# Build guest/cidata.iso from guest/cidata/{user-data.template,meta-data}
# by injecting the runner's current SSH pubkey into the user-data template.
# The resulting iso is consumed by the cloud-init NoCloud datasource on
# every guest boot to bootstrap authorized_keys for SSH.
#
# Usage: scripts/build-cidata-iso.sh [<output_iso>]

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

GUEST="${HFSSS_GUEST_DIR:-$PROJECT_DIR/guest}"
TEMPLATE="$GUEST/cidata/user-data.template"
META="$GUEST/cidata/meta-data"
SSH_KEY="${HFSSS_SSH_KEY:-${HOME}/.ssh/hfsss_qemu_key}"
OUT_ISO="${1:-$GUEST/cidata.iso}"

[ -f "$TEMPLATE" ]   || { echo "ERROR: template missing: $TEMPLATE"; exit 1; }
[ -f "$META" ]       || { echo "ERROR: meta-data missing: $META"; exit 1; }
[ -f "$SSH_KEY.pub" ] || {
    echo "ERROR: SSH pubkey missing: $SSH_KEY.pub"
    echo "       Generate via:"
    echo "         ssh-keygen -t ed25519 -f \"$SSH_KEY\" -N \"\" \\"
    echo "           && chmod 600 \"$SSH_KEY\""
    echo "       (the chmod is required: under a permissive umask the"
    echo "       key lands group/world-readable and ssh refuses to use it.)"
    exit 1
}

PUB="$(cat "$SSH_KEY.pub")"

WORKDIR="$(mktemp -d -t cidata-build.XXXXXX)"
trap 'rm -rf "$WORKDIR"' EXIT

# Substitute placeholder in template; iso volume label must be CIDATA
# (case-insensitive) for cloud-init NoCloud to find it.
sed "s|__HFSSS_RUNNER_PUBKEY__|$PUB|" "$TEMPLATE" > "$WORKDIR/user-data"
cp "$META" "$WORKDIR/meta-data"

rm -f "$OUT_ISO"
case "$(uname -s)" in
    Darwin)
        hdiutil makehybrid -iso -joliet -default-volume-name CIDATA \
            -o "$OUT_ISO" "$WORKDIR" >/dev/null
        ;;
    Linux)
        if command -v genisoimage >/dev/null 2>&1; then
            genisoimage -quiet -output "$OUT_ISO" -volid CIDATA -joliet -rock "$WORKDIR"
        elif command -v mkisofs >/dev/null 2>&1; then
            mkisofs -quiet -output "$OUT_ISO" -V CIDATA -J -R "$WORKDIR"
        elif command -v xorriso >/dev/null 2>&1; then
            xorriso -outdev "$OUT_ISO" -volid CIDATA -joliet on \
                -map "$WORKDIR" / >/dev/null 2>&1
        else
            echo "ERROR: no iso builder found; install one of:"
            echo "       apt-get install genisoimage  /  yum install genisoimage"
            echo "       or xorriso / mkisofs"
            exit 1
        fi
        ;;
    *)
        echo "ERROR: unsupported OS for iso build: $(uname -s)"
        exit 1
        ;;
esac

echo "Built $OUT_ISO authorizing pubkey:"
echo "  $PUB"
ls -lh "$OUT_ISO"
