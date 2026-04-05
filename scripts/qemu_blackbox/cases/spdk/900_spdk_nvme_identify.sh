#!/bin/bash

set -euo pipefail

CASE_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=../../lib/case.sh
. "$CASE_DIR/../../lib/case.sh"

hfsss_case_require_spdk_tool spdk_nvme_identify
hfsss_case_guest_run \
    "spdk-nvme-identify" \
    "bdf=\$(basename \"\$(readlink -f /sys/class/nvme/\$(basename \"$HFSSS_GUEST_NVME_CTRL\")/device)\") && spdk_nvme_identify -r \"trtype:PCIe traddr:\$bdf\""
