#!/bin/bash
# Bundle the privacy-clean guest assets for a GitHub release.
#
# Inputs (must already exist):
#   guest/alpine-hfsss-clean.qcow2  (from scripts/scrub-guest-image.sh)
#   guest/ovmf_vars-saved.fd
#
# Produces:
#   build/release-bundles/guest-bundle-<date>-v<ver>.tar.xz
#   build/release-bundles/guest-bundle-<date>-v<ver>.tar.xz.sha256
#
# Privacy gate: every staged asset is scanned for known PII patterns
# before compression. The script aborts on any hit.
#
# Usage:
#   scripts/build-guest-bundle.sh <version>
# Example:
#   scripts/build-guest-bundle.sh 0.001

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

VERSION="${1:?usage: $0 <version>  e.g. 0.001}"
DATE="$(date +%Y-%m-%d)"
TAG="guest-bundle-${DATE}-v${VERSION}"

GUEST="${HFSSS_GUEST_DIR:-$PROJECT_DIR/guest}"
QCOW2_SRC="$GUEST/alpine-hfsss-clean.qcow2"
OVMF_SRC="$GUEST/ovmf_vars-saved.fd"

[ -f "$QCOW2_SRC" ] || {
    echo "ERROR: $QCOW2_SRC missing — run scripts/scrub-guest-image.sh first"
    exit 1
}
[ -f "$OVMF_SRC" ] || { echo "ERROR: $OVMF_SRC missing"; exit 1; }

OUT_DIR="$PROJECT_DIR/build/release-bundles"
mkdir -p "$OUT_DIR"
WORK="$(mktemp -d -t bundle-build.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

# Stage assets into a clean tree under the bundle name. The qcow2 is
# renamed back to alpine-hfsss.qcow2 so a contributor's setup-guest
# extracts it directly into the location the runner expects.
STAGE="$WORK/$TAG"
mkdir -p "$STAGE"
cp "$QCOW2_SRC" "$STAGE/alpine-hfsss.qcow2"
cp "$OVMF_SRC"  "$STAGE/ovmf_vars-saved.fd"

# A short README so contributors who download the artifact directly
# (without setup-guest) have a sane starting point.
cat > "$STAGE/README" <<EOF
HFSSS guest bundle — $TAG

Drop into <project_root>/guest/ then run:
    scripts/build-cidata-iso.sh
    make pre-checkin

Contents:
    alpine-hfsss.qcow2     — Alpine Linux 3.21 aarch64, scrubbed for distribution
    ovmf_vars-saved.fd     — UEFI persistent vars matching the qcow2 boot order
    README                 — this file

The cidata.iso (cloud-init bootstrap) is generated locally by
scripts/build-cidata-iso.sh from a template + your local SSH pubkey.
It is NOT included in this bundle.

Need vmlinuz-lts / initramfs-lts / modloop-lts for scripts/run_qemu_nvme.sh?
Fetch from upstream Alpine; see scripts/run_qemu_nvme.sh header for URLs.
EOF

# Privacy gate. Patterns picked up from the previous iso build that
# leaked the maintainer's user/hostname/known-key fingerprints. Add
# entries when you spot something new in scrub output.
PII_PATTERNS=(
    'zifeng'
    'Mac-Studio'
    'MacBook'
    '@apple'
    'IDS6KAwJV1SRwwIpy97k1LWauR4tE5Zk1zR0kngOqNPm'
    'IDaYehXzoilj2W3m4NP0J1JGw17Mks1A49CbeGomYkWi'
)

echo "[bundle] running PII scan..."
hits=0
for f in "$STAGE/alpine-hfsss.qcow2" "$STAGE/ovmf_vars-saved.fd"; do
    for pat in "${PII_PATTERNS[@]}"; do
        if strings -n 6 "$f" 2>/dev/null | grep -F "$pat" >/dev/null; then
            count="$(strings -n 6 "$f" 2>/dev/null | grep -cF "$pat" || true)"
            echo "  ❌ $f contains '$pat' ($count occurrence(s))"
            hits=$((hits + 1))
        fi
    done
done
if [ "$hits" -gt 0 ]; then
    echo
    echo "[bundle] aborting: $hits PII leak(s) found."
    echo "        Re-run scripts/scrub-guest-image.sh, then this script."
    echo "        If a pattern is a false positive, edit PII_PATTERNS"
    echo "        in scripts/build-guest-bundle.sh."
    exit 2
fi
echo "  ok — no PII patterns matched"

TARBALL="$OUT_DIR/$TAG.tar.xz"
echo "[bundle] compressing -> $TARBALL"
tar -C "$WORK" -cJf "$TARBALL" "$TAG"

SHA="$TARBALL.sha256"
( cd "$OUT_DIR" && shasum -a 256 "$TAG.tar.xz" > "$(basename "$SHA")" )

echo
echo "[bundle] outputs:"
ls -lh "$TARBALL" "$SHA" | sed 's/^/    /'
echo
echo "[bundle] SHA256:"
sed 's/^/    /' "$SHA"
echo
echo "[bundle] tag: $TAG"
echo "[bundle] DONE — upload via scripts/upload-guest-release.sh or manual gh release create."
