#!/bin/bash
# One-shot guest-asset setup: download the latest released bundle,
# verify SHA-256, extract into guest/, and build cidata.iso for the
# current user. End state: `make pre-checkin` is ready to run.
#
# Tag conventions: guest-bundle-<YYYY-MM-DD>-v<MAJOR.MINOR>
#
# Usage:
#   scripts/setup-guest.sh                  # latest published tag
#   scripts/setup-guest.sh <full-tag-name>  # pinned tag

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"

GUEST="${HFSSS_GUEST_DIR:-$PROJECT_DIR/guest}"
GH_REPO="${HFSSS_GH_REPO:-AlvinYangZF/hfsss-simulator}"
TAG="${1:-}"

mkdir -p "$GUEST"

command -v gh >/dev/null 2>&1 || {
    echo "ERROR: gh CLI not installed (https://cli.github.com)"; exit 1;
}

if [ -z "$TAG" ]; then
    # Pick the most recent guest-bundle-* tag from the GitHub release list.
    TAG="$(gh release list --repo "$GH_REPO" --limit 50 --json tagName --jq \
            '[.[] | select(.tagName | startswith("guest-bundle-"))][0].tagName' \
            2>/dev/null || true)"
fi
[ -n "$TAG" ] || {
    echo "ERROR: no guest-bundle-* release found in $GH_REPO"; exit 1;
}
echo "[setup] tag: $TAG"

WORK="$(mktemp -d -t setup-guest.XXXXXX)"
trap 'rm -rf "$WORK"' EXIT

echo "[setup] downloading $TAG.tar.xz + $TAG.tar.xz.sha256..."
gh release download "$TAG" --repo "$GH_REPO" --dir "$WORK" \
    --pattern "$TAG.tar.xz" --pattern "$TAG.tar.xz.sha256" \
    >/dev/null

[ -f "$WORK/$TAG.tar.xz" ]        || { echo "ERROR: download missing tarball";       exit 1; }
[ -f "$WORK/$TAG.tar.xz.sha256" ] || { echo "ERROR: download missing sha256 sidecar"; exit 1; }

echo "[setup] verifying sha256..."
( cd "$WORK" && shasum -a 256 -c "$TAG.tar.xz.sha256" ) >/dev/null

echo "[setup] extracting to $GUEST/"
tar -C "$WORK" -xJf "$WORK/$TAG.tar.xz"
# Bundle stages files under <tag>/; flatten into guest/
cp "$WORK/$TAG/alpine-hfsss.qcow2" "$GUEST/alpine-hfsss.qcow2"
cp "$WORK/$TAG/ovmf_vars-saved.fd" "$GUEST/ovmf_vars-saved.fd"

# Build a fresh cidata.iso authorizing the runner's local SSH key.
"$SCRIPT_DIR/build-cidata-iso.sh"

echo
echo "[setup] guest/ now contains:"
ls -lh "$GUEST/alpine-hfsss.qcow2" "$GUEST/ovmf_vars-saved.fd" "$GUEST/cidata.iso" \
    | sed 's/^/    /'
echo
echo "[setup] DONE — try: make pre-checkin"
